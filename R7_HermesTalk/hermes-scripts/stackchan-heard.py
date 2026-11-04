#!/usr/bin/env python3
"""stackchan-heard.py: route script for the robot-voice webhook (tool server 0.8.0, sketch v0.16.0).

Hermes runs it after checking the webhook signature and before the model starts, with the
payload {"clip": N} on stdin. It asks stackchan_tools.py (Unix socket, user-only) for that
clip's words and prints the payload with "heard" added, so the prompt can carry the words
and the model does not spend a call on hear_clip. On any problem "heard" says
"(not transcribed)" and the prompt tells the model to call hear_clip. Never drops a webhook,
never prints anything but the payload.

Install: copy to ~/.hermes/scripts/ (Hermes runs route scripts only from there).
"""
import json
import socket
import sys
from pathlib import Path

SOCK = Path.home() / ".hermes" / "stackchan" / "heard.sock"
TIMEOUT_S = 8  # the robot waits 10 s for the webhook reply
FALLBACK = "(not transcribed)"


def heard(clip):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(TIMEOUT_S)
        s.connect(str(SOCK))
        s.sendall((json.dumps({"clip": clip}) + "\n").encode())
        reply = json.loads(s.makefile("r", encoding="utf-8").readline())
    return reply["text"] if reply.get("ok") and reply.get("text") else FALLBACK


def main():
    payload = json.load(sys.stdin)
    clip = payload.get("clip")
    words = FALLBACK
    if isinstance(clip, int) and not isinstance(clip, bool) and 1 <= clip <= 999999999:
        try:
            words = heard(clip)
        except (OSError, ValueError, KeyError):
            pass
    print(json.dumps({**payload, "heard": words}))


if __name__ == "__main__":
    main()
