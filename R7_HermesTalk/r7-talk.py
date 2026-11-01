"""r7-talk.py — spoken commands for the StackChan (milestone R7a-2). Standard library only.

On each head tap: record 5 s, transcribe LOCALLY with whisper.cpp, match the words against a
fixed command list, act, and confirm aloud. No AI service is involved; nothing leaves this Mac
(the "what do you see" description is made by the local gemma3 model).

  look left / right / up / down      head to that pose (within 15° of centre)
  look ahead / look forward / centre head back to centre
  nod                                nod once
  say <words>                        the robot says the words (up to 80 characters)
  what do you see                    one photo, described locally, first sentences spoken
  camera on / camera off             camera view on the robot's own screen (2-minute auto-off)
  what can you do / help             the robot lists the commands

The whole utterance must be a command (a polite "please" or "Stack Chan" is allowed), so a
question such as "look up the weather" is not mistaken for one; R7b will send those to Hermes.

Usage:
  python3 -I r7-talk.py            listen for taps until Ctrl-C (never started automatically)
  python3 -I r7-talk.py --test     offline self-test of the command matcher; no robot contact
  python3 -I r7-talk.py --plan "look to the left please"   show what a phrase would do; offline

Limits: one tap at a time (taps during a reply are dropped), at most 20 replies an hour.
Transcripts and descriptions are shown only in this terminal; logs/talk.log gets one line per
tap (time, tap number, command name, outcome) without the words. Uses the token in the macOS
Keychain; never prints it.
"""
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
CONF = HERE / "r7.conf"
LOG = HERE / "logs" / "talk.log"

HEAR = PROJECT / "R5_HermesListen" / "r5-hear.sh"
SAY = PROJECT / "R2_HermesGreeting" / "r2-say.sh"
HEAD = PROJECT / "R3_HermesMove" / "r3-head.sh"
LOOK = PROJECT / "R34_HermesMoveCamera" / "r34-look.sh"
VIEW = HERE / "r7-view.sh"

POLL_S = 1
REC_SECS = 5
MAX_PER_HOUR = 20
SAY_MAX = 80
CLIP_CHARS = 70            # one spoken clip; about 5 s with the Alex voice (robot limit ~6 s)
DESCRIBE_CLIPS = 3         # at most this many clips of a description are spoken
SAY_COOLDOWN_S = 5.2       # robot refuses /say within 5 s of the previous clip starting
MOVE_COOLDOWN_S = 3.2      # robot refuses moves closer than 3 s apart
REC_COOLDOWN_S = 10.2      # robot refuses a recording within 10 s of the previous one ending

# Absolute poses in whole degrees (yaw, pitch), as in stackchan_tools.py.
# Negative yaw is the robot's left (R6b test, session local-f885204a…).
CENTRE = (0, 30)
POSES = {
    "look_left": (-15, 30),
    "look_right": (15, 30),
    "look_up": (0, 45),
    "look_down": (0, 15),
    "centre": CENTRE,
}
NOD_DOWN = (0, 20)

HELP_TEXT = ("I can look left, right, up or down, look ahead, nod, say something, "
             "tell you what I see, and turn my camera on or off.")

# ---------- command matching (pure functions; tested by --test) ----------

FILLER_START = re.compile(r"^(?:(?:hey|hi|ok|okay|so|um|uh|now)\s+)*"
                          r"(?:(?:stack\s*chan|stacked\s*chan|stack\s*chen|robot)\s+)?"
                          r"(?:(?:please|can you|could you|would you|will you)\s+)*")
FILLER_END = re.compile(r"(?:\s+(?:please|now|thanks|thank you|stack\s*chan|stacked\s*chan))+$")

DIRECTIONS = {"left": "look_left", "right": "look_right", "up": "look_up", "upward": "look_up",
              "upwards": "look_up", "down": "look_down", "downward": "look_down",
              "downwards": "look_down"}

PATTERNS = [
    (re.compile(r"(?:look|turn|face|turn your head)\s+(?:to\s+)?(?:the\s+|your\s+)?"
                r"(left|right|up|upward|upwards|down|downward|downwards)"), "direction"),
    (re.compile(r"(?:look|face|turn)\s+(?:straight\s+)?(?:ahead|forward|forwards|straight|"
                r"to the (?:center|centre|front)|(?:at\s+)?me)|(?:center|centre)(?: yourself)?|"
                r"look (?:center|centre)"), "centre"),
    (re.compile(r"nod(?: your head)?(?: once)?|give (?:me )?a nod"), "nod"),
    (re.compile(r"(?:turn (?:on )?(?:the |your )?camera(?: on)?|(?:the |your )?camera on|"
                r"start (?:the |your )?camera|show (?:me )?(?:the |your )?camera(?: view)?|"
                r"open (?:the |your )?camera)"), "camera_on"),
    (re.compile(r"(?:turn (?:off )?(?:the |your )?camera off|turn off (?:the |your )?camera|"
                r"(?:the |your )?camera off|stop (?:the |your )?camera|close (?:the |your )?camera|"
                r"hide (?:the |your )?camera(?: view)?)"), "camera_off"),
    (re.compile(r"what (?:do|can) you see|what are you looking at|look around|take a look|"
                r"describe what you see|tell me what you see"), "look"),
    (re.compile(r"what can you do|help|what are your commands|list (?:your )?commands"), "help"),
]
SAY_PATTERN = re.compile(r"(?:say|repeat after me|repeat)\s*,?\s+(.+)", re.IGNORECASE)


def clean(text):
    """Whisper text -> letters, digits, spaces and . , ! ? ' - only (r2-say's charset).
    Drops [BLANK_AUDIO]-style markers and (..) sound notes; colons become spaces."""
    text = re.sub(r"\[[^\]]*\]|\([^)]*\)", " ", text)
    text = text.replace("’", "'").replace("‘", "'").replace(":", " ")
    text = re.sub(r"[^A-Za-z0-9 .,!?'-]", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return re.sub(r"^[^A-Za-z0-9]+", "", text)


def normalise(text):
    """Lower case, punctuation removed, polite fillers at either end removed."""
    t = re.sub(r"[^a-z0-9' ]", " ", clean(text).lower())
    t = re.sub(r"\s+", " ", t).strip()
    for _ in range(2):  # "please could you ..." or "okay stack chan please ..."
        t = FILLER_START.sub("", t)
        t = FILLER_END.sub("", t).strip()
    return t


def match_command(text):
    """Returns (name, argument). name is one of look_left, look_right, look_up, look_down,
    centre, nod, say, look, camera_on, camera_off, help, empty, unknown."""
    if not clean(text):
        return "empty", None
    # "say" keeps the speaker's own wording (case and punctuation) for the argument
    stripped = clean(text)
    m = re.match(r"^(?:(?:hey|ok|okay)\s*,?\s+)?(?:(?:stack\s*chan|stacked\s*chan)\s*,?\s+)?"
                 r"(?:(?:please|can you|could you)\s+)*", stripped, re.IGNORECASE)
    rest = stripped[m.end():] if m else stripped
    sm = SAY_PATTERN.fullmatch(rest)
    if sm:
        words = re.sub(r"^[^A-Za-z0-9]+", "", sm.group(1)).strip()
        return ("say", words) if words else ("unknown", None)

    t = normalise(text)
    if not t:
        return "empty", None
    for pattern, name in PATTERNS:
        pm = pattern.fullmatch(t)
        if pm:
            if name == "direction":
                return DIRECTIONS[pm.group(1)], None
            return name, None
    return "unknown", None


def cut_words(text, n):
    """At most n characters, whole words only."""
    if len(text) <= n:
        return text
    cut = text[:n]
    return cut.rsplit(" ", 1)[0] if " " in cut else cut


def clips_for(text, n=CLIP_CHARS, max_clips=None):
    """Split text into clips of at most n characters at sentence, then word, boundaries."""
    clips = []
    for sentence in re.split(r"(?<=[.!?])\s+", clean(text)):
        while sentence:
            part = cut_words(sentence, n)
            clips.append(part)
            sentence = sentence[len(part):].strip()
    clips = [c for c in clips if re.match(r"^[A-Za-z0-9]", c)]
    return clips[:max_clips] if max_clips else clips


# ---------- self-test (offline) ----------

SAMPLES = [
    ("Look left.", ("look_left", None)),
    ("Look to the left, please.", ("look_left", None)),
    ("Stack Chan, look right.", ("look_right", None)),
    ("Stacked Chan look up", ("look_up", None)),
    ("Look down.", ("look_down", None)),
    ("Turn to your right.", ("look_right", None)),
    ("Look ahead.", ("centre", None)),
    ("Look straight ahead.", ("centre", None)),
    ("Look at me.", ("centre", None)),
    ("Center.", ("centre", None)),
    ("Centre yourself please.", ("centre", None)),
    ("Nod.", ("nod", None)),
    ("Can you nod your head?", ("nod", None)),
    ("Say hello to everyone.", ("say", "hello to everyone.")),
    ("Please say, Good morning!", ("say", "Good morning!")),
    ("Repeat after me, I like robots.", ("say", "I like robots.")),
    ("Say", ("unknown", None)),
    ("What do you see?", ("look", None)),
    ("Stack Chan, what can you see?", ("look", None)),
    ("Take a look.", ("look", None)),
    ("Camera on.", ("camera_on", None)),
    ("Turn on the camera.", ("camera_on", None)),
    ("Turn the camera on please.", ("camera_on", None)),
    ("Show me your camera.", ("camera_on", None)),
    ("Camera off.", ("camera_off", None)),
    ("Turn off the camera.", ("camera_off", None)),
    ("Turn the camera off.", ("camera_off", None)),
    ("Stop the camera.", ("camera_off", None)),
    ("What can you do?", ("help", None)),
    ("Help.", ("help", None)),
    ("Look up the weather in Philadelphia.", ("unknown", None)),
    ("What is the capital of France?", ("unknown", None)),
    ("Can you look left and then right?", ("unknown", None)),
    ("The camera on the table is old.", ("unknown", None)),
    ("I don't want to nod off.", ("unknown", None)),
    ("[BLANK_AUDIO]", ("empty", None)),
    ("(wind blowing)", ("empty", None)),
    ("", ("empty", None)),
]


def self_test():
    failures = 0
    for text, expected in SAMPLES:
        got = match_command(text)
        if got != expected:
            failures += 1
            print(f"FAIL {text!r}: got {got}, expected {expected}")
    clip_cases = [
        ("Short.", ["Short."]),
        ("A desk with a laptop. The room is bright! Is that a plant?",
         ["A desk with a laptop.", "The room is bright!", "Is that a plant?"]),
        ("word " * 30, None),  # checked below: every clip <= CLIP_CHARS, words intact
        ("The image shows a desk: a lamp (on) and a mug [unclear].", None),
    ]
    for text, expected in clip_cases:
        clips = clips_for(text)
        ok = all(0 < len(c) <= CLIP_CHARS and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 .,!?'-]*", c)
                 for c in clips)
        if expected is not None:
            ok = ok and clips == expected
        if text.startswith("word"):
            ok = ok and " ".join(clips).split() == text.split()
        if not ok:
            failures += 1
            print(f"FAIL clips_for({text[:30]!r}...): {clips}")
    if len(clips_for("One. Two. Three. Four. Five.", max_clips=DESCRIBE_CLIPS)) != DESCRIBE_CLIPS:
        failures += 1
        print("FAIL max_clips")
    total = len(SAMPLES) + len(clip_cases) + 1
    print(f"{total - failures}/{total} passed")
    return failures == 0


# ---------- robot access ----------

def now():
    return time.strftime("%H:%M:%S")


def log(seq, command, outcome):
    """One line per tap: never the transcript or any spoken words."""
    LOG.parent.mkdir(exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} tap={seq} {command} {outcome}\n")


def last_line(out):
    lines = out.strip().splitlines()
    return lines[-1] if lines else ""


def first_line(out):
    lines = out.strip().splitlines()
    return lines[0] if lines else "no reply"


class Robot:
    def __init__(self):
        conf = CONF.read_text(encoding="utf-8")
        ip = re.search(r"^ROBOT_IP=(\d{1,3}(?:\.\d{1,3}){3})$", conf, re.M)
        port = re.search(r"^ROBOT_PORT=(\d{1,5})$", conf, re.M)
        if not ip or not port:
            sys.exit("error: ROBOT_IP/ROBOT_PORT missing or invalid in r7.conf")
        self.base = f"http://{ip.group(1)}:{port.group(1)}"
        proc = subprocess.run(["security", "find-generic-password", "-s", "stackchan-r1-token", "-w"],
                              capture_output=True, text=True)
        if proc.returncode != 0 or not proc.stdout.strip():
            sys.exit("error: Keychain entry 'stackchan-r1-token' not found")
        self._token = proc.stdout.strip()  # kept in memory only; never printed or logged
        self.last_say_start = None
        self.last_move_end = None
        self.last_rec_end = None

    def request(self, method, path):
        """Returns the JSON reply as a dict, or None if the robot did not answer."""
        req = urllib.request.Request(self.base + path, method=method,
                                     headers={"Authorization": f"Bearer {self._token}"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            try:
                return json.loads(e.read().decode("utf-8", "replace"))
            except ValueError:
                return {"error": f"HTTP {e.code}"}
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def run(self, script, *args, timeout):
        """Run one project script; no shell, fixed paths. Returns (exit code, output)."""
        try:
            proc = subprocess.run([str(script), *args], cwd=PROJECT, capture_output=True,
                                  text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "error: timed out"
        return proc.returncode, (proc.stdout + proc.stderr).strip()

    # --- speech ---

    def _say_clip(self, text):
        """Speak one clip; waits for the robot's say cooldown first and until the clip ends
        afterwards (the robot answers when playback starts). Returns None or why not."""
        if self.last_say_start is not None:
            wait = SAY_COOLDOWN_S - (time.monotonic() - self.last_say_start)
            if wait > 0:
                time.sleep(wait)
        code, out = self.run(SAY, text, timeout=40)
        if code != 0:
            return out
        if last_line(out) != "HTTP 200":
            return f"{first_line(out)} ({last_line(out)})"
        self.last_say_start = time.monotonic()
        m = re.search(r'"bytes":(\d+)', out)
        nbytes = int(m.group(1)) if m else 196652  # unknown: assume the longest clip
        time.sleep((nbytes - 44) / 32000 + 0.3)  # 16 kHz mono 16-bit after a 44-byte header
        return None

    def speak(self, text, max_clips=None):
        """Speak text as one or more clips. Returns True if every clip was played."""
        for clip in clips_for(text, max_clips=max_clips):
            why = self._say_clip(clip)
            if why and "limit" in why and " " in clip:  # too long once spoken: halve it
                half = cut_words(clip, len(clip) // 2)
                why = self._say_clip(half) or self._say_clip(clip[len(half):].strip())
            if why:
                print(f"{now()}   robot did not speak: {why}")
                return False
        return True

    # --- head ---

    def _move_once(self, yaw, pitch):
        """One move with one retry on cooldown/busy. Returns None or the robot's reason."""
        reason = "no reply"
        for attempt in range(2):
            if self.last_move_end is not None:
                wait = MOVE_COOLDOWN_S - (time.monotonic() - self.last_move_end)
                if wait > 0:
                    time.sleep(wait)
            code, out = self.run(HEAD, "move", str(yaw), str(pitch), timeout=30)
            status = last_line(out)
            if status.startswith("HTTP "):
                self.last_move_end = time.monotonic()
            if code == 0 and status == "HTTP 200" and '"moved":true' in out:
                return None
            reason = f"{first_line(out)} ({status or 'no HTTP reply'})"
            if attempt == 0 and status in ("HTTP 429", "HTTP 409"):
                continue
            break
        return reason

    def move(self, yaw, pitch):
        """Move to an absolute pose; via a halfway point if the step is over the 30° limit
        (tap gestures can leave the head tilted further). Returns None or why not."""
        why = self._move_once(yaw, pitch)
        if why and "step too large" in why:
            st = self.request("GET", "/status") or {}
            y, p = st.get("last_yaw"), st.get("last_pitch")
            if isinstance(y, int) and isinstance(p, int):
                why = (self._move_once((int(y / 10) + yaw) // 2, (int(p / 10) + pitch) // 2)
                       or self._move_once(yaw, pitch))
        return why


# ---------- acting on one tap ----------

def act(robot, command, arg):
    """Carry out one command. Returns (outcome, recentre_after)."""
    if command in POSES:
        why = robot.move(*POSES[command])
        if why:
            print(f"{now()}   head did not move: {why}")
            robot.speak("Sorry, I could not move my head.")
            return "not-moved", False
        words = {"look_left": "Looking left.", "look_right": "Looking right.", "look_up": "Looking up.",
                 "look_down": "Looking down.", "centre": "Looking ahead."}[command]
        return ("done" if robot.speak(words) else "moved-not-spoken"), False

    if command == "nod":
        why = robot.move(*NOD_DOWN) or robot.move(*CENTRE)
        if why:
            print(f"{now()}   nod incomplete: {why}")
            return "not-moved", True
        return "done", False

    if command == "say":
        words = cut_words(arg, SAY_MAX)
        return ("done" if robot.speak(words, max_clips=1) else "not-spoken"), True

    if command == "look":
        robot.speak("Let me look.")
        print(f"{now()}   taking one photo; describing it locally (this can take a minute)")
        code, out = robot.run(LOOK, timeout=150)
        if code != 0:
            print(f"{now()}   no description: {last_line(out)}")
            robot.speak("Sorry, I could not see anything.")
            return "no-photo", True
        lines = out.splitlines()
        desc = " ".join(lines[2:]).strip()  # after the "photo:" and "description by" lines
        print(f"{now()}   {lines[0] if lines else ''}")
        print(f"{now()}   described: {desc}")
        return ("done" if desc and robot.speak(desc, max_clips=DESCRIBE_CLIPS) else "not-spoken"), True

    if command in ("camera_on", "camera_off"):
        code, out = robot.run(VIEW, "on" if command == "camera_on" else "off", timeout=20)
        if code == 0 and last_line(out) == "HTTP 200":
            robot.speak("Camera on." if command == "camera_on" else "Camera off.")
            return "done", True
        print(f"{now()}   camera view not changed: {first_line(out)} ({last_line(out)})")
        robot.speak("Sorry, my camera did not respond.")
        return "refused", True

    if command == "help":
        return ("done" if robot.speak(HELP_TEXT) else "not-spoken"), True

    if command == "empty":
        robot.speak("Sorry, I did not catch that.")
        return "nothing-heard", True

    robot.speak("Sorry, I don't know that one yet.")
    return "not-understood", True


def handle_tap(robot, seq):
    if robot.last_rec_end is not None:
        wait = REC_COOLDOWN_S - (time.monotonic() - robot.last_rec_end)
        if wait > 0:
            print(f"{now()}   waiting {wait:.0f} s for the robot's recording cooldown")
            time.sleep(wait)
    print(f"{now()}   recording {REC_SECS} s: speak after the beep")
    code, out = robot.run(HEAR, str(REC_SECS), timeout=60)
    robot.last_rec_end = time.monotonic()  # a little after the recording really ended
    if code != 0:
        print(f"{now()}   no recording: {last_line(out)}")
        log(seq, "-", "no-recording")
        why = robot.move(*CENTRE)
        if why:
            print(f"{now()}   head not recentred: {why}")
        return
    transcript = last_line(out)
    if transcript == "(no speech recognised)":
        transcript = ""
    command, arg = match_command(transcript)
    print(f"{now()}   heard: {clean(transcript) or '(nothing recognised)'}")
    print(f"{now()}   command: {command}{' ' + repr(arg) if arg else ''}")
    outcome, recentre = act(robot, command, arg)
    if recentre:
        why = robot.move(*CENTRE)
        if why:
            print(f"{now()}   head not recentred: {why}")
            outcome += "-not-recentred"
    log(seq, command, outcome)


def drop_pending(robot, reason):
    ev = robot.request("GET", "/events")
    if ev and ev.get("tap_pending") is True and isinstance(ev.get("tap_seq"), int):
        robot.request("POST", f"/events/ack?seq={ev['tap_seq']}")
        print(f"{now()} tap #{ev['tap_seq']} ignored ({reason})")
        log(ev["tap_seq"], "-", "ignored-" + reason.replace(" ", "-"))


def listen():
    for script in (HEAR, SAY, HEAD, LOOK, VIEW):
        if not script.is_file():
            sys.exit(f"error: missing script {script}")
    robot = Robot()
    print(f"watching {robot.base} for head taps (live, {REC_SECS} s recordings); Ctrl-C to stop")
    print("say 'what can you do' after a tap to hear the commands")
    drop_pending(robot, "pending at start")

    replies = []  # monotonic times of replies in the last hour
    last_quiet = None
    fails = 0
    while True:
        ev = robot.request("GET", "/events")
        if ev and "tap_pending" in ev:
            if fails >= 3:
                print(f"{now()} robot reachable again")
            fails = 0
            quiet = ev.get("quiet")
            if quiet != last_quiet:
                if quiet:
                    print(f"{now()} robot is in QUIET mode (taps are ignored by the robot)")
                elif last_quiet is not None:
                    print(f"{now()} robot left QUIET mode")
                last_quiet = quiet
            seq = ev.get("tap_seq")
            if ev["tap_pending"] is True and isinstance(seq, int):
                robot.request("POST", f"/events/ack?seq={seq}")
                t = time.monotonic()
                replies = [r for r in replies if t - r < 3600]
                if len(replies) >= MAX_PER_HOUR:
                    print(f"{now()} tap #{seq} ignored (limit of {MAX_PER_HOUR} replies an hour reached)")
                    log(seq, "-", "ignored-hourly-limit")
                else:
                    print(f"{now()} tap #{seq}")
                    replies.append(t)
                    handle_tap(robot, seq)
                    drop_pending(robot, "arrived during the reply")
        else:
            fails += 1
            if fails == 3:
                print(f"{now()} robot not responding; still trying")
        time.sleep(POLL_S)


def main():
    args = sys.argv[1:]
    if args == ["--test"]:
        sys.exit(0 if self_test() else 1)
    if len(args) == 2 and args[0] == "--plan":
        command, arg = match_command(args[1])
        print(f"command: {command}{' ' + repr(arg) if arg else ''}")
        return
    if args:
        sys.exit("usage: r7-talk.py [--test | --plan \"words\"]")
    try:
        listen()
    except KeyboardInterrupt:
        print(f"\nstopped ({now()})")


if __name__ == "__main__":
    main()
