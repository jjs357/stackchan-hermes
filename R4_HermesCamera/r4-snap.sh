#!/bin/bash
# r4-snap.sh — ask the StackChan (R4 sketch) for ONE camera frame and save it locally.
# Uses the token in the macOS Keychain; never prints it.
#
# Usage:
#   ./r4-snap.sh            saves captures/snap-YYYYmmdd-HHMMSS.jpg (readable only by you)
#
# Images stay on this Mac. Nothing is uploaded or analysed by this script.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r4.conf"
outdir="$here/captures"
max_bytes=262144

[[ $# -eq 0 ]] || { echo "usage: $0" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r4.conf" >&2
    exit 2
fi

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

umask 077
mkdir -p "$outdir"
tmpdir="$(mktemp -d -t r4-snap)"
trap 'rm -rf "$tmpdir"' EXIT
body="$tmpdir/body"

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
meta="$(curl -sS --max-time 20 -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -o "$body" -w '%{http_code} %{content_type} %{header_json}' \
    "http://$ip:$port/camera/snap")"
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
if [[ "$ctype" != "image/jpeg" ]] || (( size < 100 || size > max_bytes )); then
    echo "error: unexpected reply (type '$ctype', $size bytes); not saved" >&2
    exit 1
fi
# JPEG files start with FF D8 and end with FF D9
if [[ "$(head -c 2 "$body" | xxd -p)" != "ffd8" || "$(tail -c 2 "$body" | xxd -p)" != "ffd9" ]]; then
    echo "error: reply is not a complete JPEG; not saved" >&2
    exit 1
fi

out="$outdir/snap-$(date +%Y%m%d-%H%M%S).jpg"
mv "$body" "$out"
capture_ms="$(printf '%s' "$rest" | sed -n 's/.*"x-capture-ms":\["\([0-9]*\)"\].*/\1/p')"
echo "saved $out ($size bytes, capture ${capture_ms:-?} ms)"
echo "HTTP $code"
