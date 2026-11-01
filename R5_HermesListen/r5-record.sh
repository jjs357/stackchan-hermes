#!/bin/bash
# r5-record.sh — ask the StackChan (R5 sketch) for ONE short microphone recording and save it.
# Uses the token in the macOS Keychain; never prints it.
#
# Usage:
#   ./r5-record.sh [secs]   secs 1..8 (default 5); saves recordings/rec-YYYYmmdd-HHMMSS.wav
#                           (readable only by you). Play it with: afplay <file>
#
# Recordings stay on this Mac. Nothing is uploaded, transcribed or analysed by this script;
# it prints only the length and signal level.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r5.conf"
outdir="$here/recordings"

secs="${1:-5}"
[[ $# -le 1 && "$secs" =~ ^[1-8]$ ]] || { echo "usage: $0 [secs 1..8]" >&2; exit 2; }
max_bytes=$(( 44 + secs * 16000 * 2 ))

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r5.conf" >&2
    exit 2
fi

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

umask 077
mkdir -p "$outdir"
tmpdir="$(mktemp -d -t r5-record)"
trap 'rm -rf "$tmpdir"' EXIT
body="$tmpdir/body"

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
meta="$(curl -sS --max-time $(( secs + 20 )) -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -o "$body" -w '%{http_code} %{content_type} %{header_json}' \
    "http://$ip:$port/mic/record?secs=$secs")"
code="${meta%% *}"
rest="${meta#* }"
ctype="${rest%% *}"

if [[ "$code" != "200" ]]; then
    # Errors are short JSON messages from the sketch
    head -c 300 "$body"; echo
    echo "HTTP $code"
    exit 1
fi

size="$(wc -c < "$body" | tr -d ' ')"
if [[ "$ctype" != "audio/wav" || "$size" -ne "$max_bytes" ]]; then
    echo "error: unexpected reply (type '$ctype', $size bytes, expected $max_bytes); not saved" >&2
    exit 1
fi
# RIFF....WAVEfmt  at the start, "data" chunk at byte 36
if [[ "$(head -c 4 "$body")" != "RIFF" || "$(dd if="$body" bs=1 skip=8 count=8 2>/dev/null)" != "WAVEfmt " ||
      "$(dd if="$body" bs=1 skip=36 count=4 2>/dev/null)" != "data" ]]; then
    echo "error: reply is not the expected WAV layout; not saved" >&2
    exit 1
fi

hdr() { printf '%s' "$rest" | sed -n "s/.*\"$1\":\[\"\([0-9]*\)\"\].*/\1/p"; }
out="$outdir/rec-$(date +%Y%m%d-%H%M%S).wav"
mv "$body" "$out"
echo "saved $out ($size bytes, ${secs} s, record $(hdr x-record-ms) ms, peak $(hdr x-peak), rms $(hdr x-rms) of 32767)"
echo "HTTP $code"
