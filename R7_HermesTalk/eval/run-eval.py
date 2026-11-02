#!/usr/bin/env python3
"""Ask each model the loaded questions through the stackchaneval profile (dry run, no robot).

usage: python3 -I run-eval.py MODEL [MODEL ...] [--only ID[,ID...]] [--ollama-url URL]

MODEL is "profile" (the model set in stackchaneval's config.yaml) or "ollama:NAME" (an Ollama
model through http://localhost:11434/v1, local or :cloud; --ollama-url changes that, e.g.
http://localhost:11435/v1 for an SSH tunnel to another Mac's Ollama). Each question becomes a dry-run clip
(run/clips/N.txt) and the exact robot-voice prompt from ~/.hermes/config.yaml, then one
`hermes -p stackchaneval chat --oneshot` run. The tool server's dry-run log (run/calls.jsonl)
shows what each run tried; results go to results/<time>.jsonl and a summary is printed.
The question text goes to the model's provider; nothing reaches the robot.
"""

import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUN = HERE / "run"
CONFIG = Path.home() / ".hermes" / "config.yaml"
OLLAMA_URL = "http://localhost:11434/v1"  # --ollama-url overrides
MAX_TURNS = "30"   # as many model calls as the incident run had: limits are the tool server's job
# Runs go through the shared gateway: an MCP server with the live one's name ("stackchan") was
# handed the live connection (2026-11-02), so the eval profile's server is "stackchandry" and
# every model must first pass a hear_clip-only preflight that lands in the dry-run log.
PREFLIGHT = "Call hear_clip with clip {clip}. Then reply with exactly the text it returned. Call no other tool."
ACTIONS = {"say", "gesture", "look", "hear", "camera_view"}


def robot_voice_prompt():
    """The robot-voice route's prompt block from the gateway config (indent-based, no YAML lib)."""
    lines = CONFIG.read_text().splitlines()
    start = lines.index("        robot-voice:")
    for i in range(start + 1, len(lines)):
        if lines[i].strip() == "prompt: |":
            body = []
            for line in lines[i + 1:]:
                if line.strip() and not line.startswith("            "):
                    break
                body.append(line[12:])
            return "\n".join(body).strip()
    sys.exit("error: robot-voice prompt not found in " + str(CONFIG))


def questions(only):
    out = []
    for line in (HERE / "questions.txt").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            qid, kind, words, expect = [p.strip() for p in line.split("|")]
            if not only or qid in only:
                out.append({"id": qid, "kind": kind, "words": words, "expect": expect})
    return out


def model_args(model):
    if model == "profile":
        return []
    if model.startswith("ollama:"):
        return ["-m", model[len("ollama:"):], "--provider", "custom", "--base-url", OLLAMA_URL]
    sys.exit(f"error: unknown model {model!r} (use profile or ollama:NAME)")


def calls_since(offset):
    path = RUN / "calls.jsonl"
    if not path.exists():
        return [], 0
    data = path.read_bytes()
    return [json.loads(l) for l in data[offset:].decode().splitlines() if l.strip()], len(data)


def ask(prompt, margs):
    return subprocess.run(["hermes", "-p", "stackchaneval", "chat", "--oneshot", "-Q", "-q", prompt,
                           "--max-turns", MAX_TURNS, *margs],
                          capture_output=True, text=True, stdin=subprocess.DEVNULL)


def main(argv):
    global OLLAMA_URL
    only = None
    if "--ollama-url" in argv:
        i = argv.index("--ollama-url")
        OLLAMA_URL = argv[i + 1]
        del argv[i:i + 2]
    if "--only" in argv:
        i = argv.index("--only")
        only = set(argv[i + 1].split(","))
        del argv[i:i + 2]
    if not argv:
        sys.exit(__doc__.strip().splitlines()[2])
    (RUN / "clips").mkdir(parents=True, exist_ok=True)
    (HERE / "results").mkdir(exist_ok=True)
    template = robot_voice_prompt()
    qs = questions(only)
    results = HERE / "results" / time.strftime("%Y%m%d-%H%M%S.jsonl")
    _, offset = calls_since(0)
    clip = int(time.time()) % 100000 * 100
    for model in argv:
        margs = model_args(model)
        for attempt in (1, 2):  # a session can start before the gateway has the server up
            clip += 1
            (RUN / "clips" / f"{clip}.txt").write_text("preflight ok\n")
            ask(PREFLIGHT.replace("{clip}", str(clip)), margs)
            calls, offset = calls_since(offset)
            (RUN / "clips" / f"{clip}.txt").unlink(missing_ok=True)
            if any(c["tool"] == "hear_clip" and c["ok"] for c in calls):
                break
        else:
            sys.exit(f"STOP: preflight for {model} did not reach the dry-run tool server")
        for q in qs:
            clip += 1
            (RUN / "clips" / f"{clip}.txt").write_text(q["words"] + "\n")
            started = time.monotonic()
            proc = ask(template.replace("{clip}", str(clip)), margs)
            secs = round(time.monotonic() - started, 1)
            calls, offset = calls_since(offset)
            (RUN / "clips" / f"{clip}.txt").unlink(missing_ok=True)
            if not any(c["tool"] == "hear_clip" for c in calls):
                sys.exit(f"STOP: {model} {q['id']} left no dry-run hear_clip record (live server?)")
            row = {"model": model, **q, "secs": secs, "exit": proc.returncode,
                   "reply": proc.stdout.strip()[-300:], "calls": calls}
            with open(results, "a", encoding="utf-8") as f:
                f.write(json.dumps(row) + "\n")
            acts = [c for c in calls if c["tool"] in ACTIONS]
            unasked = sum(1 for c in calls if c["result"].startswith("not asked"))
            over = sum(1 for c in calls if c["result"].startswith("this turn"))
            said = " / ".join(c["args"].get("text", "") for c in acts if c["tool"] == "say" and c["ok"])
            others = ",".join(c["tool"] + ("" if c["ok"] else "!") for c in acts if c["tool"] != "say")
            print(f"{model:32} {q['id']:9} {secs:6}s acts={len(acts)} unasked={unasked} over={over} "
                  f"[{others}] {said}", flush=True)
    print(f"\nfull results: {results}")


if __name__ == "__main__":
    main(sys.argv[1:])
