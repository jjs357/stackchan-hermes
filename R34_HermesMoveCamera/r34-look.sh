#!/bin/bash
# r34-look.sh — take ONE photo with the StackChan (R34 sketch), describe it with a
# LOCAL vision model (Ollama), and print only the text description.
#
# Usage:
#   ./r34-look.sh           photo is deleted after the description (the mode Hermes uses)
#   ./r34-look.sh --keep    also keep the photo and description in ../R4_HermesCamera/captures/
#                           (for your own runs; Hermes is blocked from passing --keep)
#
# Privacy design (see ../SETUP-STATUS.md): the photo goes only from the robot to this Mac and
# to Ollama at 127.0.0.1. Only the text description is printed, so a caller such as Hermes
# sees words, never the image. Uses the token in the macOS Keychain; never prints it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r34.conf"
keepdir="$here/../R4_HermesCamera/captures"
model="gemma3:latest"
ollama="http://127.0.0.1:11434"
max_bytes=262144
prompt="This photo was taken by a small desk robot's camera. In 2 to 4 sentences, describe the main objects, the setting and the lighting. If people are visible, do not guess who they are."

keep=0
if [[ "${1:-}" == "--keep" ]]; then
    keep=1
    shift
fi
[[ $# -eq 0 ]] || { echo "usage: $0 [--keep]" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r34.conf" >&2
    exit 2
fi

umask 077
tmpdir="$(mktemp -d -t r34-look)"
trap 'rm -rf "$tmpdir"' EXIT
img="$tmpdir/photo.jpg"

# 1. Check the model before taking a photo: local (not ":cloud") and able to see images
curl -sS --max-time 10 "$ollama/api/show" -d "{\"model\":\"$model\"}" -o "$tmpdir/show.json" \
    || { echo "error: Ollama is not reachable at $ollama (is the Ollama app running?)" >&2; exit 1; }
python3 -I - "$tmpdir/show.json" <<'EOF' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
if "error" in d:
    sys.exit("error: Ollama: " + str(d["error"]))
if d.get("remote_host") or d.get("remote_model"):
    sys.exit("error: model is cloud-backed; refusing to send the image")
if "vision" not in (d.get("capabilities") or []):
    sys.exit("error: model does not support images")
EOF

# 2. Take the photo (same checks as r4-snap.sh), into the private temp folder only
if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi
# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
meta="$(curl -sS --max-time 20 -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -o "$img" -w '%{http_code} %{content_type} %{header_json}' \
    "http://$ip:$port/camera/snap")"
unset token
code="${meta%% *}"
rest="${meta#* }"
ctype="${rest%% *}"

if [[ "$code" != "200" ]]; then
    # Errors are short JSON messages from the sketch
    echo "photo: not taken"
    head -c 300 "$img"; echo
    echo "HTTP $code"
    exit 1
fi
size="$(wc -c < "$img" | tr -d ' ')"
if [[ "$ctype" != "image/jpeg" ]] || (( size < 100 || size > max_bytes )); then
    echo "error: unexpected reply (type '$ctype', $size bytes); discarded" >&2
    exit 1
fi
if [[ "$(head -c 2 "$img" | xxd -p)" != "ffd8" || "$(tail -c 2 "$img" | xxd -p)" != "ffd9" ]]; then
    echo "error: reply is not a complete JPEG; discarded" >&2
    exit 1
fi
capture_ms="$(printf '%s' "$rest" | sed -n 's/.*"x-capture-ms":\["\([0-9]*\)"\].*/\1/p')"

# 3. Describe it locally; the request is built in a file, never on the command line
python3 -I - "$img" "$model" "$prompt" "$tmpdir/req.json" <<'EOF'
import base64, json, sys
img, model, prompt, out = sys.argv[1:5]
with open(img, "rb") as f:
    b64 = base64.b64encode(f.read()).decode()
json.dump({"model": model, "prompt": prompt, "images": [b64], "stream": False,
           "options": {"temperature": 0.2}}, open(out, "w"))
EOF
start=$(date +%s)
curl -sS --max-time 600 "$ollama/api/generate" --data-binary @"$tmpdir/req.json" -o "$tmpdir/resp.json"
secs=$(( $(date +%s) - start ))
python3 -I - "$tmpdir/resp.json" "$tmpdir/desc.txt" <<'EOF' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
if "error" in d:
    sys.exit("error: Ollama: " + str(d["error"]))
text = (d.get("response") or "").strip()
if not text:
    sys.exit("error: empty description")
open(sys.argv[2], "w").write(text + "\n")
EOF

# 4. Keep (own runs only) or let the trap delete everything
if (( keep )); then
    mkdir -p "$keepdir"
    stamp="$(date +%Y%m%d-%H%M%S)"
    mv "$img" "$keepdir/snap-$stamp.jpg"
    cp "$tmpdir/desc.txt" "$keepdir/snap-$stamp.txt"
    kept="kept in R4_HermesCamera/captures as snap-$stamp.jpg"
else
    kept="deleted after description"
fi

echo "photo: taken ($size bytes, capture ${capture_ms:-?} ms), $kept"
echo "description by $model (local, ${secs} s):"
cat "$tmpdir/desc.txt"
