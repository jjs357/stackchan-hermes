#!/bin/bash
# r7-clip.sh — take ONE hold-to-talk clip from the StackChan (R7 sketch v0.10.0), transcribe it
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
voice_secs=5
wav_bytes=$(( 44 + voice_secs * 16000 * 2 ))

clip="${1:-}"
[[ $# -eq 1 && "$clip" =~ ^[1-9][0-9]{0,8}$ ]] || { echo "usage: $0 <clip 1..999999999>" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r7.conf" >&2
    exit 2
fi

# 1. Check the transcriber before taking the clip (taking it wipes it on the robot)
command -v whisper-cli >/dev/null || { echo "error: whisper-cli not installed" >&2; exit 1; }
[[ -f "$model" ]] || { echo "error: model missing: $model" >&2; exit 1; }
if [[ "$(shasum -a 256 "$model" | cut -d' ' -f1)" != "$model_sha256" ]]; then
    echo "error: model checksum does not match the published value; refusing to use it" >&2
    exit 1
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
    -o "$audio" -w '%{http_code} %{content_type} %{header_json}' \
    "http://$ip:$port/voice/take?clip=$clip")"
unset token
code="${meta%% *}"
rest="${meta#* }"
ctype="${rest%% *}"

if [[ "$code" != "200" ]]; then
    # Errors are short JSON messages from the sketch
    echo "clip: not taken"
    head -c 300 "$audio"; echo
    echo "HTTP $code"
    exit 1
fi
size="$(wc -c < "$audio" | tr -d ' ')"
if [[ "$ctype" != "audio/wav" || "$size" -ne "$wav_bytes" ]]; then
    echo "error: unexpected reply (type '$ctype', $size bytes, expected $wav_bytes); discarded" >&2
    exit 1
fi
if [[ "$(head -c 4 "$audio")" != "RIFF" || "$(dd if="$audio" bs=1 skip=8 count=8 2>/dev/null)" != "WAVEfmt " ||
      "$(dd if="$audio" bs=1 skip=36 count=4 2>/dev/null)" != "data" ]]; then
    echo "error: reply is not the expected WAV layout; discarded" >&2
    exit 1
fi
hdr() { printf '%s' "$rest" | sed -n "s/.*\"$1\":\[\"\([0-9]*\)\"\].*/\1/p"; }

# 3. Transcribe offline; the trap deletes the audio and transcript afterwards
start=$(date +%s)
whisper-cli -m "$model" -f "$audio" -l en -t "$(sysctl -n hw.physicalcpu)" -nt -np \
    --prompt "$hint" -otxt -of "$tmpdir/transcript" >/dev/null 2>"$tmpdir/whisper.log" \
    || { echo "error: whisper-cli failed" >&2; exit 1; }
secs_taken=$(( $(date +%s) - start ))
text="$(sed 's/^[[:space:]]*//' "$tmpdir/transcript.txt" | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
[[ -n "$text" ]] || text="(no speech recognised)"

echo "clip $clip: ${voice_secs} s (peak $(hdr x-peak), rms $(hdr x-rms) of 32767), deleted after transcription"
echo "transcript by whisper.cpp base.en (local, ${secs_taken} s):"
echo "$text"
