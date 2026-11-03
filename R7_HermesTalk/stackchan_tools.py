"""stackchan_tools.py — robot-only tools for the Hermes "stackchan" profile (R6b; R7b adds camera_view).

A minimal MCP server over stdio (JSON lines, protocol 2024-11-05), standard library only.
Each tool runs one of the project's tested scripts with checked arguments, never a shell:

  say(text)        ../R2_HermesGreeting/r2-say.sh  — speak up to 80 plain characters
  gesture(name)    ../R3_HermesMove/r3-head.sh     — fixed poses within ±15° of centre (sides ±13°)
  look()           ../R34_HermesMoveCamera/r34-look.sh — local gemma3 text; photo deleted
  hear(secs)       ../R5_HermesListen/r5-hear.sh   — local whisper text; audio deleted
  camera_view(on)  r7-view.sh                      — live camera picture on the robot's screen
  hear_clip(clip)  r7-clip.sh                      — a tap-to-talk clip as local whisper text;
                                                     the robot hands it out once, audio deleted

Follow-up context: every robot turn is a fresh Hermes session, so this server remembers the
last few exchanges (words heard by hear_clip, words spoken by say) for TALK_WINDOW_S and adds
them to the next hear_clip result. Kept in memory only: never logged or written to disk, and
gone when the window passes or the gateway restarts this server.

Fast transcription: this server starts whisper.cpp's whisper-server (model checked against its
published checksum) on 127.0.0.1:WHISPER_PORT from r7.conf and stops it on exit, so the model
stays loaded between clips. r7-clip.sh uses it and falls back to whisper-cli if it is not up.
Its output is discarded (it could contain transcripts); it runs in an empty private folder.

Turn rules (0.7.0, after a weaker model ran away on 2026-11-01): they hold whatever the model
does. A turn starts when hear_clip hands out a clip, or with the first robot action after
TURN_S of quiet. Each turn allows TURN_MAX_ACTIONS calls of say, gesture, look, hear and
camera_view, refused attempts included. Once they are used up, one say is still allowed if the
turn has not spoken yet, so the person always hears a reply. look and camera_view on run
only if the words heard just now ask for the camera (SEE_RE); hear only if they ask to listen
(LISTEN_RE). A turn with no clip (robot-text route, a chat) has no heard words, so it cannot
use the camera or mic.
A new clip starts a new turn: an older run still going shares the new turn's budget.

Dry run (--dry-run DIR, for comparing models): no robot, no whisper, no scripts. hear_clip N
returns DIR/clips/N.txt (then deletes it), look returns a fixed fake description, the other
tools pretend to succeed. Turn rules and the call limit still apply, but each hear_clip
starts afresh (call count and follow-up memory cleared) so questions do not affect each other. Every call, with its
arguments and result, goes to DIR/calls.jsonl, since only test questions pass through it.

The robot still enforces its own token, limits, cooldowns and QUIET mode. This server adds
a call limit (30 per 10 minutes) and logs one line per call (time, tool, result) without content.
Moves and camera_view retry while the robot answers 409 (busy: audio playing), since say returns
before the robot has finished speaking.
"""
import atexit
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
LOG = Path(__file__).resolve().parent / "logs" / "tools.log"
# The gateway keeps one server process across many sessions, so the limit is a sliding window
MAX_CALLS = 30  # R7b: a spoken turn uses hear_clip + an action + a spoken confirmation
WINDOW_S = 600  # at most MAX_CALLS calls in any 10 minutes

WHISPER_MODEL = PROJECT / "R5_HermesListen/models/ggml-base.en.bin"
WHISPER_SHA256 = "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"  # as r7-clip.sh
WHISPER_HINT = "Stack Chan, StackChan robot."
whisper_proc = None

SAY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 .,!?'-]*$")
SAY_MAX = 80

# Absolute poses in whole degrees (yaw, pitch), all within ±15° of centre (0, 30).
# Positive yaw turns to the robot's right (seen in the R6b test, session local-b46bd0ce…).
# Sides are ±13°, not ±15°: the servos land up to ~1° off target, so a left-to-right swing
# of 30° was sometimes over the robot's 30° step limit (v0.11.0 test, 2026-11-01).
CENTRE = (0, 30)
POSES = {
    "centre": CENTRE,
    "look_up": (0, 45),
    "look_down": (0, 15),
    "look_left": (-13, 30),
    "look_right": (13, 30),
}
NOD_DOWN = (0, 20)
MOVE_COOLDOWN_S = 3.2  # robot refuses moves closer than 3 s apart
BUSY_WAIT_S = 1.5      # wait between tries while the robot answers 409 (audio playing)
BUSY_TRIES = 6         # a full 80-character clip plays for about 6 s

TOOLS = [
    {
        "name": "say",
        "description": "Speak a short sentence aloud through the robot's speaker (quiet volume). "
                       "Plain English only: letters, digits, spaces and . , ! ? ' - ; at most 80 characters.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string", "maxLength": SAY_MAX}},
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "gesture",
        "description": "Move the robot's head to a small fixed pose: centre, look_up, look_down, "
                       "look_left, look_right, or nod (down and back to centre).",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string", "enum": sorted(list(POSES) + ["nod"])}},
            "required": ["name"],
            "additionalProperties": False,
        },
    },
    {
        "name": "look",
        "description": "Take one photo with the robot's camera and return a short text description "
                       "made locally on the Mac. The photo itself is deleted and never returned.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "hear",
        "description": "Record 1 to 8 seconds with the robot's microphone (it beeps first) and return "
                       "the text transcript made locally on the Mac. The audio is deleted.",
        "inputSchema": {
            "type": "object",
            "properties": {"secs": {"type": "integer", "minimum": 1, "maximum": 8}},
            "required": ["secs"],
            "additionalProperties": False,
        },
    },
    {
        "name": "camera_view",
        "description": "Turn the live camera picture on the robot's screen on or off. It turns itself "
                       "off after 2 minutes. Nothing is recorded or sent anywhere.",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean"}},
            "required": ["on"],
            "additionalProperties": False,
        },
    },
    {
        "name": "hear_clip",
        "description": "Get the words of a voice clip someone recorded by touching the robot's head "
                       "(the clip number comes with the request). Returns the text transcript made "
                       "locally on the Mac, plus any exchanges from the last 2 minutes so follow-up "
                       "questions make sense. Each clip can be taken once; the audio is deleted.",
        "inputSchema": {
            "type": "object",
            "properties": {"clip": {"type": "integer", "minimum": 1, "maximum": 999999999}},
            "required": ["clip"],
            "additionalProperties": False,
        },
    },
]

call_times = []  # time.monotonic() of recent calls
last_move_end = None  # time.monotonic() of the last reply to a move request

ACTION_TOOLS = {"say", "gesture", "look", "hear", "camera_view"}
TURN_MAX_ACTIONS = 4  # e.g. look_left + look + two says
TURN_S = 90
SEE_RE = re.compile(r"\b(see|seeing|saw|camera|photos?|pictures?|view|describe|look(ing)? (at|around)|in front|"
                    r"who('s|’s| is) (here|there|in)|any(one|body) (here|there))\b", re.I)
LISTEN_RE = re.compile(r"\b(listen|hear|record)\b", re.I)
turn_state = {"t": None, "heard": "", "actions": 0, "said": False}  # heard: words of this turn's clip, memory only

DRY_DIR = None  # set by --dry-run DIR
DRY_LOOK = "A desk with a laptop, a mug and a window behind it. No people are visible."
DRY_HEAR = "Nothing was heard."

TALK_WINDOW_S = 120  # an exchange is kept this long after its last words
TALK_MAX_TURNS = 3
talk_turns = []  # oldest first: {"t": time.monotonic() of last words, "heard": str, "said": [str]}


def log(tool, outcome, secs=None):
    """One line per event; 0.7.1: time to the millisecond, and how long a tool call ran (the
    gap from one call's end to the next call's start is the model thinking)."""
    LOG.parent.mkdir(exist_ok=True)
    now = time.time()
    took = "" if secs is None else f" {secs:.2f}s"
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now))}.{int(now % 1 * 1000):03d} "
                f"{tool} {outcome}{took}\n")


def conf_value(name):
    """One NAME=value line from r7.conf (parsed, never sourced)."""
    for line in (PROJECT / "R7_HermesTalk/r7.conf").read_text().splitlines():
        if line.startswith(name + "="):
            return line.split("=", 1)[1].strip()
    return ""


def ensure_whisper():
    """Start whisper-server unless it is running; on any problem leave r7-clip.sh to whisper-cli."""
    global whisper_proc
    if DRY_DIR:
        return
    if whisper_proc is not None and whisper_proc.poll() is None:
        return
    whisper_proc = None
    port = conf_value("WHISPER_PORT")
    binary = shutil.which("whisper-server")
    if not port.isdigit() or not binary or not WHISPER_MODEL.is_file():
        log("whisper-server", "not-started")
        return
    with open(WHISPER_MODEL, "rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != WHISPER_SHA256:
            log("whisper-server", "bad-model")
            return
    threads = subprocess.run(["sysctl", "-n", "hw.physicalcpu"], capture_output=True, text=True).stdout.strip()
    workdir = tempfile.mkdtemp(prefix="whisper-server-")  # empty: nothing to serve, nothing kept
    whisper_proc = subprocess.Popen(
        [binary, "--host", "127.0.0.1", "--port", port, "-m", str(WHISPER_MODEL), "-l", "en", "-nt",
         "-t", threads if threads.isdigit() else "4", "--prompt", WHISPER_HINT, "--public", workdir],
        cwd=workdir, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    atexit.register(stop_whisper, whisper_proc, workdir)
    log("whisper-server", "started")


def stop_whisper(proc, workdir):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    shutil.rmtree(workdir, ignore_errors=True)


def run(script, *args, timeout):
    """Run one project script; returns (ok, stdout). No shell, fixed paths."""
    if DRY_DIR:
        return dry_run(script, *args)
    proc = subprocess.run([str(PROJECT / script), *args], cwd=PROJECT, capture_output=True,
                          text=True, timeout=timeout)
    return proc.returncode == 0, proc.stdout.strip()


def dry_run(script, *args):
    """Dry run: answer like the robot scripts would, without running them or touching the robot."""
    if script.endswith("r7-clip.sh"):
        clip = DRY_DIR / "clips" / f"{args[0]}.txt"
        if not clip.is_file():
            return False, "error: no such clip"
        words = clip.read_text().strip()
        clip.unlink()  # handed out once, like the robot
        return True, words
    if script.endswith("r3-head.sh"):
        return True, '{"moved":true}\nHTTP 200'
    if script.endswith("r7-view.sh"):
        return True, '{"camera_view":%s}\nHTTP 200' % ("true" if args[0] == "on" else "false")
    if script.endswith("r34-look.sh"):
        return True, DRY_LOOK
    if script.endswith("r5-hear.sh"):
        return True, DRY_HEAR
    return True, "ok"


def dry_log(params, ok, text):
    """Dry run: one JSON line per call with its arguments and result (test questions only)."""
    with open(DRY_DIR / "calls.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps({"time": time.strftime("%Y-%m-%d %H:%M:%S"), "tool": params.get("name"),
                            "args": params.get("arguments"), "ok": ok, "result": text[:200]}) + "\n")


def split_reply(out):
    """Scripts that print the robot's JSON reply and then an "HTTP nnn" line; returns (status, body)."""
    lines = out.splitlines()
    status = lines[-1] if lines and lines[-1].startswith("HTTP ") else ""
    return status, ("\n".join(lines[:-1]) if status else out)


def refusal(status, body, fallback):
    try:
        reason = json.loads(body).get("error") or fallback
    except (ValueError, AttributeError):
        reason = body.splitlines()[0] if body else "no reply"
    return f"{status or 'no HTTP reply'}: {reason}"


def move(yaw, pitch):
    """One head move. Succeeds only on HTTP 200 with "moved":true (r3-head.sh exits 0 on any
    HTTP reply, so its exit code alone is not enough). Waits out the robot's cooldown first,
    retries once if the robot still answers 429 (e.g. a manual r3-head.sh run just before),
    and keeps retrying for a few seconds while it answers 409 (still speaking)."""
    global last_move_end
    cooldown_retried = False
    for _ in range(BUSY_TRIES):
        if last_move_end is not None:
            wait = MOVE_COOLDOWN_S - (time.monotonic() - last_move_end)
            if wait > 0:
                time.sleep(wait)
        ok, out = run("R3_HermesMove/r3-head.sh", "move", str(yaw), str(pitch), timeout=30)
        status, body = split_reply(out)
        if status:
            last_move_end = time.monotonic()  # any reply may follow a powered attempt
        if ok and status == "HTTP 200" and '"moved":true' in body:
            return
        if status == "HTTP 429" and not cooldown_retried:
            cooldown_retried = True
            continue  # the wait at the top of the loop covers the cooldown
        if status == "HTTP 409":
            time.sleep(BUSY_WAIT_S)
            continue
        break
    error = StepTooLarge if status == "HTTP 400" and "step too large" in body else ToolError
    raise error("robot refused the move (" + refusal(status, body, "move did not complete") + ")")


class ToolError(Exception):
    pass


class StepTooLarge(ToolError):
    pass


def tool_say(args):
    text = str(args.get("text", "")).replace("’", "'").replace("‘", "'").strip()
    if not text or len(text) > SAY_MAX or not SAY_RE.match(text):
        raise ToolError("text must be 1-80 plain characters: letters, digits, spaces and . , ! ? ' -")
    ok, out = run("R2_HermesGreeting/r2-say.sh", text, timeout=40)
    if not ok:
        raise ToolError(f"robot did not speak: {out.splitlines()[0] if out else 'no reply'}")
    prune_talk()
    if talk_turns:
        talk_turns[-1]["said"].append(text)
        talk_turns[-1]["t"] = time.monotonic()
    return "spoken"


def tool_gesture(args):
    name = args.get("name")
    if name == "nod":
        move(*NOD_DOWN)
        move(*CENTRE)  # move() waits out the cooldown
        return "nodded"
    if name not in POSES:
        raise ToolError("unknown gesture")
    try:
        move(*POSES[name])
    except StepTooLarge:
        move(*CENTRE)  # too far from where the head is now: go via centre
        move(*POSES[name])
    return f"head at {name}"


def tool_look(args):
    ok, out = run("R34_HermesMoveCamera/r34-look.sh", timeout=120)
    if not ok:
        raise ToolError(f"no photo: {out.splitlines()[-1] if out else 'no reply'}")
    return out


def tool_hear(args):
    secs = args.get("secs")
    if not isinstance(secs, int) or isinstance(secs, bool) or not 1 <= secs <= 8:
        raise ToolError("secs must be an integer 1..8")
    ok, out = run("R5_HermesListen/r5-hear.sh", str(secs), timeout=60)
    if not ok:
        raise ToolError(f"no recording: {out.splitlines()[-1] if out else 'no reply'}")
    return out


def tool_camera_view(args):
    on = args.get("on")
    if not isinstance(on, bool):
        raise ToolError("on must be true or false")
    for _ in range(BUSY_TRIES):
        ok, out = run("R7_HermesTalk/r7-view.sh", "on" if on else "off", timeout=30)
        status, body = split_reply(out)
        if ok and status == "HTTP 200" and f'"camera_view":{"true" if on else "false"}' in body:
            return "camera view on (turns itself off after 2 minutes)" if on else "camera view off"
        if status != "HTTP 409":
            break
        time.sleep(BUSY_WAIT_S)
    raise ToolError("robot refused the camera view (" + refusal(status, body, "view did not change") + ")")


def tool_hear_clip(args):
    clip = args.get("clip")
    if not isinstance(clip, int) or isinstance(clip, bool) or not 1 <= clip <= 999999999:
        raise ToolError("clip must be an integer 1..999999999")
    ensure_whisper()  # restarts it if it has stopped; this clip may still use whisper-cli
    ok, out = run("R7_HermesTalk/r7-clip.sh", str(clip), timeout=60)
    if not ok:
        raise ToolError(f"no clip: {out.splitlines()[-1] if out else 'no reply'}")
    prune_talk()
    earlier = []
    for turn in talk_turns:
        earlier.append(f"Person: {turn['heard']}")
        if turn["said"]:
            earlier.append("Robot: " + " ".join(turn["said"]))
    turn_state.update(t=time.monotonic(), heard=out, actions=0, said=False)
    talk_turns.append({"t": time.monotonic(), "heard": out, "said": []})
    del talk_turns[:-TALK_MAX_TURNS]
    if not earlier:
        return out
    return ("Earlier in this conversation (last 2 minutes, oldest first):\n" + "\n".join(earlier)
            + "\n\nJust now the person said: " + out)


def turn_refusal(name, args):
    """Apply the turn rules to one robot action; returns a reason to refuse, or None."""
    now = time.monotonic()
    if turn_state["t"] is None or now - turn_state["t"] >= TURN_S:
        turn_state.update(t=now, heard="", actions=0, said=False)
    if turn_state["actions"] >= TURN_MAX_ACTIONS:
        if name == "say" and not turn_state["said"]:
            turn_state["said"] = True  # the person always gets one spoken reply
            return None
        return (f"this turn has used its {TURN_MAX_ACTIONS} robot actions; "
                "stop now and end with a one-line text reply")
    turn_state["actions"] += 1
    if (name == "look" or (name == "camera_view" and args.get("on") is True)) \
            and not SEE_RE.search(turn_state["heard"]):
        return ("not asked: camera not used because the person did not ask to see anything "
                "(the camera works; do not say it is broken or blocked)")
    if name == "hear" and not LISTEN_RE.search(turn_state["heard"]):
        return ("not asked: microphone not used because the person did not ask the robot to listen "
                "(it works; do not say it is broken)")
    if name == "say":
        turn_state["said"] = True
    return None


def prune_talk():
    now = time.monotonic()
    talk_turns[:] = [t for t in talk_turns if now - t["t"] < TALK_WINDOW_S]


HANDLERS = {"say": tool_say, "gesture": tool_gesture, "look": tool_look, "hear": tool_hear,
            "camera_view": tool_camera_view, "hear_clip": tool_hear_clip}


def call_tool(params):
    name = params.get("name")
    args = params.get("arguments") or {}
    if name not in HANDLERS:
        return False, f"unknown tool {name!r}"
    if not isinstance(args, dict):
        return False, "arguments must be an object"
    now = time.monotonic()
    if DRY_DIR and name == "hear_clip":
        call_times.clear()  # dry run: each question meets a fresh robot (limit and follow-ups)
        talk_turns.clear()
    call_times[:] = [t for t in call_times if now - t < WINDOW_S]
    if len(call_times) >= MAX_CALLS:
        log(name, "refused-limit")
        return False, f"call limit reached ({MAX_CALLS} per {WINDOW_S // 60} minutes); try again later"
    call_times.append(now)
    if name in ACTION_TOOLS:
        reason = turn_refusal(name, args)
        if reason:
            log(name, "refused-turn" if reason.startswith("this turn") else "refused-unasked")
            return False, reason
    started = time.monotonic()
    try:
        text = HANDLERS[name](args)
    except ToolError as e:
        log(name, "error", time.monotonic() - started)
        return False, str(e)
    except subprocess.TimeoutExpired:
        log(name, "timeout", time.monotonic() - started)
        return False, "timed out"
    log(name, "ok", time.monotonic() - started)
    return True, text


def reply(msg_id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    print(json.dumps(msg), flush=True)


def main():
    global DRY_DIR, LOG, MOVE_COOLDOWN_S
    if sys.argv[1:2] == ["--dry-run"] and len(sys.argv) == 3:
        DRY_DIR = Path(sys.argv[2]).resolve()
        LOG = DRY_DIR / "tools.log"  # keep test runs out of the robot log
        MOVE_COOLDOWN_S = 0
    elif sys.argv[1:]:
        sys.exit("usage: stackchan_tools.py [--dry-run DIR]")
    else:
        ensure_whisper()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        method = msg.get("method")
        msg_id = msg.get("id")
        if msg_id is None:
            continue  # notifications (e.g. notifications/initialized) need no reply
        if method == "initialize":
            reply(msg_id, {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                           "serverInfo": {"name": "stackchan-tools", "version": "0.7.1"}})
        elif method == "ping":
            reply(msg_id, {})
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            ok, text = call_tool(msg.get("params") or {})
            if DRY_DIR:
                dry_log(msg.get("params") or {}, ok, text)
            reply(msg_id, {"isError": not ok, "content": [{"type": "text", "text": text}]})
        else:
            reply(msg_id, error={"code": -32601, "message": f"method not found: {method}"})


if __name__ == "__main__":
    main()
