#!/bin/bash
# r7-clip.sh — take ONE talk clip from the StackChan (R7 sketch v0.12.0: 1–8 s), transcribe it
# LOCALLY with whisper.cpp, and print only the text. Used by the hear_clip tool (R7b).
#
# Usage:
#   ./r7-clip.sh <clip>      clip number from the robot's webhook (1..999999999)
#
# The robot hands each clip out once and wipes it, so a second call for the same clip gets
# HTTP 404. Privacy as r5-hear.sh: the audio goes only from the robot to this Mac and to
# whisper-cli, which runs offline, and is deleted here after transcription. Only the
# transcript is printed. Uses the token in the macOS Keychain; never prints it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r7.conf"
model="$here/../R5_HermesListen/models/ggml-base.en.bin"
model_sha256="a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"
hint="Stack Chan, StackChan robot."
min_bytes=$(( 44 + 1 * 16000 * 2 ))  # the robot ends a clip when speech stops: 1..8 s
max_bytes=$(( 44 + 8 * 16000 * 2 ))

clip="${1:-}"
[[ $# -eq 1 && "$clip" =~ ^[1-9][0-9]{0,8}$ ]] || { echo "usage: $0 <clip 1..999999999>" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
wport="$(sed -n 's/^WHISPER_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r7.conf" >&2
    exit 2
fi

now() { perl -MTime::HiRes=time -e 'printf "%d", time * 1000'; }

# 1. Check the transcriber before taking the clip (taking it wipes it on the robot). A running
#    whisper-server checked the model when stackchan_tools.py started it, so the 0.7 s checksum
#    is only needed for the whisper-cli fallback.
server_up=false
if [[ "$wport" =~ ^[0-9]{1,5}$ ]] && curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$wport/"; then
    server_up=true
else
    command -v whisper-cli >/dev/null || { echo "error: whisper-cli not installed" >&2; exit 1; }
    [[ -f "$model" ]] || { echo "error: model missing: $model" >&2; exit 1; }
    if [[ "$(shasum -a 256 "$model" | cut -d' ' -f1)" != "$model_sha256" ]]; then
        echo "error: model checksum does not match the published value; refusing to use it" >&2
        exit 1
    fi
fi

# 2. Take the clip, into a private temp folder only
umask 077
tmpdir="$(mktemp -d -t r7-clip)"
trap 'rm -rf "$tmpdir"' EXIT
audio="$tmpdir/audio.wav"

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi
# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
meta="$(curl -sS --max-time 15 -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -o "$audio" -w '%{http_code} %{content_type} %{time_total} %{header_json}' \
    "http://$ip:$port/voice/take?clip=$clip")"
unset token
code="${meta%% *}"
rest="${meta#* }"
ctype="${rest%% *}"
rest="${rest#* }"
download_s="${rest%% *}"

if [[ "$code" != "200" ]]; then
    # Errors are short JSON messages from the sketch
    echo "clip: not taken"
    head -c 300 "$audio"; echo
    echo "HTTP $code"
    exit 1
fi
size="$(wc -c < "$audio" | tr -d ' ')"
if [[ "$ctype" != "audio/wav" ]] || (( size < min_bytes || size > max_bytes || size % 2 )); then
    echo "error: unexpected reply (type '$ctype', $size bytes, expected $min_bytes..$max_bytes); discarded" >&2
    exit 1
fi
le32() { od -An -tu4 -j "$1" -N4 "$audio" | tr -d ' '; }
if [[ "$(head -c 4 "$audio")" != "RIFF" || "$(dd if="$audio" bs=1 skip=8 count=8 2>/dev/null)" != "WAVEfmt " ||
      "$(dd if="$audio" bs=1 skip=36 count=4 2>/dev/null)" != "data" ||
      "$(le32 4)" != "$(( size - 8 ))" || "$(le32 24)" != "16000" || "$(le32 40)" != "$(( size - 44 ))" ]]; then
    echo "error: reply is not the expected WAV layout; discarded" >&2
    exit 1
fi
hdr() { printf '%s' "$rest" | sed -n "s/.*\"$1\":\[\"\([0-9]*\)\"\].*/\1/p"; }

# 3. Transcribe offline; the trap deletes the audio and transcript afterwards. The whisper-server
#    that stackchan_tools.py keeps on 127.0.0.1 has the model loaded already; whisper-cli
#    (which loads it each time) is the fallback.
start=$(now)
engine="whisper-server"
if ! $server_up ||
   ! curl -sS --max-time 30 -o "$tmpdir/transcript.txt" -F file=@"$audio" -F response_format=text \
        -F temperature=0 "http://127.0.0.1:$wport/inference" 2>/dev/null ||
   grep -q '^{"error"' "$tmpdir/transcript.txt"; then
    engine="whisper-cli"
    if [[ "$(shasum -a 256 "$model" | cut -d' ' -f1)" != "$model_sha256" ]]; then
        echo "error: model checksum does not match the published value; refusing to use it" >&2
        exit 1
    fi
    whisper-cli -m "$model" -f "$audio" -l en -t "$(sysctl -n hw.physicalcpu)" -nt -np \
        --prompt "$hint" -otxt -of "$tmpdir/transcript" >/dev/null 2>"$tmpdir/whisper.log" \
        || { echo "error: whisper-cli failed" >&2; exit 1; }
fi
transcribe_ms=$(( $(now) - start ))
text="$(sed 's/^[[:space:]]*//' "$tmpdir/transcript.txt" | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
[[ -n "$text" ]] || text="(no speech recognised)"

clip_ms=$(( (size - 44) * 1000 / 32000 ))
echo "clip $clip: $(( clip_ms / 1000 )).$(( clip_ms % 1000 / 100 )) s (peak $(hdr x-peak), rms $(hdr x-rms) of 32767), deleted after transcription"
echo "transcript by whisper.cpp base.en ($engine, local; download ${download_s} s, transcription $(( transcribe_ms / 1000 )).$(( transcribe_ms % 1000 / 100 )) s):"
echo "$text"
