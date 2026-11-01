"""stackchan_tools.py — robot-only tools for the Hermes "stackchan" profile (milestone R6b).

A minimal MCP server over stdio (JSON lines, protocol 2024-11-05), standard library only.
Each tool runs one of the project's tested scripts with checked arguments, never a shell:

  say(text)        ../R2_HermesGreeting/r2-say.sh  — speak up to 80 plain characters
  gesture(name)    ../R3_HermesMove/r3-head.sh     — fixed poses within ±15° of centre
  look()           ../R34_HermesMoveCamera/r34-look.sh — local gemma3 text; photo deleted
  hear(secs)       ../R5_HermesListen/r5-hear.sh   — local whisper text; audio deleted

The robot still enforces its own token, limits, cooldowns and QUIET mode. This server adds
a call limit (12 per 10 minutes) and logs one line per call (time, tool, result) without content.
"""
import json
import re
import subprocess
import sys
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
LOG = Path(__file__).resolve().parent / "logs" / "tools.log"
# The gateway keeps one server process across many sessions, so the limit is a sliding window
MAX_CALLS = 12
WINDOW_S = 600  # at most MAX_CALLS calls in any 10 minutes

SAY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 .,!?'-]*$")
SAY_MAX = 80

# Absolute poses in whole degrees (yaw, pitch), all within ±15° of centre (0, 30).
# Positive yaw turns to the robot's right (seen in the R6b test, session local-b46bd0ce…).
CENTRE = (0, 30)
POSES = {
    "centre": CENTRE,
    "look_up": (0, 45),
    "look_down": (0, 15),
    "look_left": (-15, 30),
    "look_right": (15, 30),
}
NOD_DOWN = (0, 20)
MOVE_COOLDOWN_S = 3.2  # robot refuses moves closer than 3 s apart

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
]

call_times = []  # time.monotonic() of recent calls
last_move_end = None  # time.monotonic() of the last reply to a move request


def log(tool, outcome):
    LOG.parent.mkdir(exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {tool} {outcome}\n")


def run(script, *args, timeout):
    """Run one project script; returns (ok, stdout). No shell, fixed paths."""
    proc = subprocess.run([str(PROJECT / script), *args], cwd=PROJECT, capture_output=True,
                          text=True, timeout=timeout)
    return proc.returncode == 0, proc.stdout.strip()


def move(yaw, pitch):
    """One head move. Succeeds only on HTTP 200 with "moved":true (r3-head.sh exits 0 on any
    HTTP reply, so its exit code alone is not enough). Waits out the robot's cooldown first,
    and retries once if the robot still answers 429 (e.g. a manual r3-head.sh run just before)."""
    global last_move_end
    for attempt in range(2):
        if last_move_end is not None:
            wait = MOVE_COOLDOWN_S - (time.monotonic() - last_move_end)
            if wait > 0:
                time.sleep(wait)
        ok, out = run("R3_HermesMove/r3-head.sh", "move", str(yaw), str(pitch), timeout=30)
        lines = out.splitlines()
        status = lines[-1] if lines and lines[-1].startswith("HTTP ") else ""
        body = "\n".join(lines[:-1]) if status else out
        if status:
            last_move_end = time.monotonic()  # any reply may follow a powered attempt
        if ok and status == "HTTP 200" and '"moved":true' in body:
            return
        if status == "HTTP 429" and attempt == 0:
            continue  # the wait at the top of the loop covers the cooldown
        try:
            reason = json.loads(body).get("error") or "move did not complete"
        except (ValueError, AttributeError):
            reason = lines[0] if lines else "no reply"
        raise ToolError(f"robot refused the move ({status or 'no HTTP reply'}): {reason}")


class ToolError(Exception):
    pass


def tool_say(args):
    text = str(args.get("text", "")).replace("’", "'").replace("‘", "'").strip()
    if not text or len(text) > SAY_MAX or not SAY_RE.match(text):
        raise ToolError("text must be 1-80 plain characters: letters, digits, spaces and . , ! ? ' -")
    ok, out = run("R2_HermesGreeting/r2-say.sh", text, timeout=40)
    if not ok:
        raise ToolError(f"robot did not speak: {out.splitlines()[0] if out else 'no reply'}")
    return "spoken"


def tool_gesture(args):
    name = args.get("name")
    if name == "nod":
        move(*NOD_DOWN)
        move(*CENTRE)  # move() waits out the cooldown
        return "nodded"
    if name not in POSES:
        raise ToolError("unknown gesture")
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


HANDLERS = {"say": tool_say, "gesture": tool_gesture, "look": tool_look, "hear": tool_hear}


def call_tool(params):
    name = params.get("name")
    args = params.get("arguments") or {}
    if name not in HANDLERS:
        return False, f"unknown tool {name!r}"
    if not isinstance(args, dict):
        return False, "arguments must be an object"
    now = time.monotonic()
    call_times[:] = [t for t in call_times if now - t < WINDOW_S]
    if len(call_times) >= MAX_CALLS:
        log(name, "refused-limit")
        return False, f"call limit reached ({MAX_CALLS} per {WINDOW_S // 60} minutes); try again later"
    call_times.append(now)
    try:
        text = HANDLERS[name](args)
    except ToolError as e:
        log(name, "error")
        return False, str(e)
    except subprocess.TimeoutExpired:
        log(name, "timeout")
        return False, "timed out"
    log(name, "ok")
    return True, text


def reply(msg_id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    print(json.dumps(msg), flush=True)


def main():
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
                           "serverInfo": {"name": "stackchan-tools", "version": "0.1.0"}})
        elif method == "ping":
            reply(msg_id, {})
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            ok, text = call_tool(msg.get("params") or {})
            reply(msg_id, {"isError": not ok, "content": [{"type": "text", "text": text}]})
        else:
            reply(msg_id, error={"code": -32601, "message": f"method not found: {method}"})


if __name__ == "__main__":
    main()
