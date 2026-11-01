#!/bin/bash
# r4-describe.sh — describe one saved StackChan photo with a LOCAL vision model (Ollama).
#
# Usage:
#   ./r4-describe.sh [--print] [captures/snap-....jpg]
#       no file given  -> the newest image in captures/
#       --print        -> also show the description in the terminal
#
# The description is saved next to the image as <name>.txt (readable only by you).
# The image goes only to Ollama at 127.0.0.1 on this Mac. The script refuses to run
# unless the model is stored locally (not an Ollama ":cloud" model) and supports vision.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
capdir="$here/captures"
model="gemma3:latest"
ollama="http://127.0.0.1:11434"
max_bytes=262144
prompt="This photo was taken by a small desk robot's camera. In 2 to 4 sentences, describe the main objects, the setting and the lighting. If people are visible, do not guess who they are."

print=0
if [[ "${1:-}" == "--print" ]]; then
    print=1
    shift
fi
[[ $# -le 1 ]] || { echo "usage: $0 [--print] [image.jpg]" >&2; exit 2; }

if [[ $# -eq 1 ]]; then
    img="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
else
    img="$(ls -t "$capdir"/snap-*.jpg 2>/dev/null | head -1 || true)"
    [[ -n "$img" ]] || { echo "error: no images in $capdir" >&2; exit 2; }
fi

# Only images this project saved: inside captures/, named like r4-snap.sh names them
if [[ "$(dirname "$img")" != "$capdir" || ! "$(basename "$img")" =~ ^snap-[0-9]{8}-[0-9]{6}\.jpg$ ]]; then
    echo "error: image must be a snap-YYYYmmdd-HHMMSS.jpg file in $capdir" >&2
    exit 2
fi
[[ -f "$img" ]] || { echo "error: $img not found" >&2; exit 2; }
size="$(wc -c < "$img" | tr -d ' ')"
if (( size < 100 || size > max_bytes )) || [[ "$(head -c 2 "$img" | xxd -p)" != "ffd8" ]]; then
    echo "error: $img is not a JPEG of the expected size" >&2
    exit 2
fi

umask 077
tmpdir="$(mktemp -d -t r4-describe)"
trap 'rm -rf "$tmpdir"' EXIT

# Refuse cloud-backed or non-vision models
# /api/show no longer marks cloud models (Ollama 0.40.13), so /api/tags and the name are checked too
curl -sS --max-time 10 "$ollama/api/show" -d "{\"model\":\"$model\"}" -o "$tmpdir/show.json" \
    && curl -sS --max-time 10 "$ollama/api/tags" -o "$tmpdir/tags.json" \
    || { echo "error: Ollama is not reachable at $ollama (is the Ollama app running?)" >&2; exit 1; }
python3 -I - "$tmpdir/show.json" "$tmpdir/tags.json" "$model" <<'EOF' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
model = sys.argv[3]
if "error" in d:
    sys.exit("error: Ollama: " + str(d["error"]))
names = {model, model if ":" in model else model + ":latest"}
tags = [m for m in json.load(open(sys.argv[2])).get("models", []) if m.get("name") in names]
if not tags:
    sys.exit("error: model is not in Ollama's local list; refusing to send the image")
if (model.split(":")[-1].endswith("cloud") or d.get("remote_host") or d.get("remote_model")
        or any(m.get("remote_host") or m.get("remote_model") for m in tags)):
    sys.exit("error: model is cloud-backed; refusing to send the image")
if "vision" not in (d.get("capabilities") or []):
    sys.exit("error: model does not support images")
EOF

# Build the request in a file (base64 image), never on the command line
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

txt="${img%.jpg}.txt"
python3 -I - "$tmpdir/resp.json" "$txt" "$model" <<'EOF'
import json, sys
resp, txt, model = sys.argv[1:4]
d = json.load(open(resp))
if "error" in d:
    sys.exit("error: Ollama: " + str(d["error"]))
text = (d.get("response") or "").strip()
if not text:
    sys.exit("error: empty description")
with open(txt, "w") as f:
    f.write(text + "\n")
print(f"model {model}, {d.get('eval_count', '?')} tokens, {len(text.split())} words")
EOF

echo "saved $txt (${secs} s)"
if (( print )); then
    echo "---"
    cat "$txt"
fi
