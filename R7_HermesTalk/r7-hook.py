#!/usr/bin/env python3
"""Send one signed POST to the Hermes gateway's StackChan webhook routes (R7b test tool).

usage: python3 -I r7-hook.py ("words the person said" | --clip N) [--bad-sig] [--twice] [--host H]

Words go to the robot-text route as {"text": ...}; --clip N goes to the robot-voice route as
{"clip": N}, exactly what the robot sends after a hold-to-talk recording. Signs with generic
HMAC V2 (X-Webhook-Timestamp + X-Webhook-Signature-V2 = hex HMAC-SHA256 of "<ts>.<body>").
The secret comes from the Keychain (service stackchan-webhook-secret), is kept in memory only
and never printed. --bad-sig signs with a wrong key (expect 401); --twice resends the identical
request with the same X-Request-ID (expect a duplicate reply the second time).
"""

import hashlib
import hmac
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

ROUTES = {"text": "/p/stackchan/webhooks/robot-text", "clip": "/p/stackchan/webhooks/robot-voice"}
USAGE = __doc__.strip().splitlines()[2]


def secret():
    proc = subprocess.run(["security", "find-generic-password", "-s", "stackchan-webhook-secret", "-w"],
                          capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        sys.exit("error: Keychain entry 'stackchan-webhook-secret' not found")
    return proc.stdout.strip()


def post(url, body, headers):
    req = urllib.request.Request(url, data=body, method="POST", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")
    except OSError as e:
        return None, str(e)


def take_value(argv, flag):
    if flag not in argv:
        return None
    i = argv.index(flag)
    if i + 1 >= len(argv):
        sys.exit(USAGE)
    value = argv[i + 1]
    del argv[i:i + 2]
    return value


def main(argv):
    argv = list(argv)
    host = take_value(argv, "--host") or "127.0.0.1"
    clip = take_value(argv, "--clip")
    flags = {a for a in argv if a.startswith("--")}
    words = [a for a in argv if not a.startswith("--")]
    if flags - {"--bad-sig", "--twice"} or (clip is None) == (len(words) != 1) or len(words) > 1:
        sys.exit(USAGE)
    if clip is not None:
        if not clip.isdigit() or not 1 <= int(clip) <= 999999999:
            sys.exit("error: --clip needs a number 1..999999999")
        body, route = json.dumps({"clip": int(clip)}).encode(), ROUTES["clip"]
    else:
        body, route = json.dumps({"text": words[0]}).encode(), ROUTES["text"]
    key = "wrong-key" if "--bad-sig" in flags else secret()
    ts = str(int(time.time()))
    sig = hmac.new(key.encode(), ts.encode() + b"." + body, hashlib.sha256).hexdigest()
    headers = {"Content-Type": "application/json", "X-Webhook-Timestamp": ts,
               "X-Webhook-Signature-V2": sig, "X-Request-ID": uuid.uuid4().hex}
    url = f"http://{host}:8644{route}"
    for _ in range(2 if "--twice" in flags else 1):
        status, text = post(url, body, headers)
        print(f"HTTP {status} {text}")


if __name__ == "__main__":
    main(sys.argv[1:])
