#!/bin/bash
# r2-say.sh — speak one short phrase on the StackChan (R2 sketch).
# Speech is synthesised offline on this Mac with macOS `say`, then uploaded to POST /say.
# Uses the token in the macOS Keychain; never prints it.
# Usage: ./r2-say.sh ["Short phrase"]   (default: a fixed greeting)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r2.conf"
text="${1:-Hello! I am Stack Chan.}"

# Plain short text only: letters, digits, spaces and basic punctuation; must not start with "-"
if ! [[ ${#text} -le 80 && "$text" =~ ^[A-Za-z0-9][A-Za-z0-9\ .,!?\'-]*$ ]]; then
    echo "error: phrase must be 1-80 chars of letters, digits, spaces or .,!?'-" >&2
    exit 2
fi

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r2.conf" >&2
    exit 2
fi

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

# Private temp folder, removed whole on exit (say needs a .wav name, so not mktemp's file)
tmpdir="$(mktemp -d -t r2-say)"
trap 'rm -rf "$tmpdir"' EXIT
wav="$tmpdir/greeting.wav"

# 16 kHz mono 16-bit PCM, the format the sketch accepts; writes a file, plays nothing here
say -o "$wav" --file-format=WAVE --data-format=LEI16@16000 --channels=1 "$text"

# Soft word endings were lost on the robot's speaker ("Tap received." heard as "Tap re"):
# raise the voice (which peaks near 55% of full scale) to -1 dBFS, with 0.25 s of silence
# first while the amplifier starts. The sketch's volume (128 since v0.8.1) does the rest.
# sox keeps the format; without sox, send as is.
if command -v sox >/dev/null; then
    sox "$wav" "$tmpdir/padded.wav" pad 0.25 norm -1 && mv "$tmpdir/padded.wav" "$wav"
fi

size="$(stat -f %z "$wav")"
if (( size > 196608 )); then
    echo "error: audio is $size bytes; limit is 196608 (about 6 s)" >&2
    exit 2
fi

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
curl -sS --max-time 15 \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -F "file=@$wav;type=audio/wav" \
    -w '\nHTTP %{http_code}\n' \
    "http://$ip:$port/say"
