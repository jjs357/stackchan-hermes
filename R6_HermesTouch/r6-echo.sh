#!/bin/bash
# r6-echo.sh — touch-to-hear for the StackChan (R6 sketch). On each head tap: record a few
# seconds, transcribe LOCALLY with whisper.cpp, have the robot say what it heard, then
# return the head to centre. No AI service is involved; nothing leaves this Mac.
#
# Usage:
#   ./r6-echo.sh --dry-run   no recording: on a tap, print what would happen, say
#                            "I felt a tap on my head." and recentre the head
#   ./r6-echo.sh             live: record 5 s per tap (robot shows MIC REC and beeps first)
# Runs until you press Ctrl-C (it is never started automatically).
#
# Limits: one tap at a time (taps during a reply are ignored), at most 20 replies an hour.
# The transcript is shown only in this terminal; logs/echo.log gets one line per tap
# (time, tap number, outcome) without content. Uses the token in the macOS Keychain;
# never prints it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
project="$(cd "$here/.." && pwd)"
conf="$here/r6.conf"
logfile="$here/logs/echo.log"
hear="$project/R5_HermesListen/r5-hear.sh"
say="$project/R2_HermesGreeting/r2-say.sh"
head="$project/R3_HermesMove/r3-head.sh"

poll_secs=1
rec_secs=5
max_per_hour=20
prefix="I heard, "
centre_yaw=0
centre_pitch=30
move_cooldown=3.2

dry=0
case "${1:-}" in
    --dry-run) dry=1; shift ;;
esac
[[ $# -eq 0 ]] || { echo "usage: $0 [--dry-run]" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r6.conf" >&2
    exit 2
fi
for s in "$hear" "$say" "$head"; do
    [[ -x "$s" ]] || { echo "error: missing script $s" >&2; exit 2; }
done
if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
robot() {  # robot <GET|POST> <path>
    curl -sS --max-time 5 -X "$1" -H @<(printf 'Authorization: Bearer %s\n' "$token") \
        "http://$ip:$port$2"
}
field() {  # field <json> <name> -> value of a flat JSON field
    printf '%s' "$1" | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p"
}
now() { date +%H:%M:%S; }
log() {  # log <tap#> <outcome>   (never content)
    mkdir -p "$(dirname "$logfile")"
    printf '%s tap=%s %s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" "$([[ $dry == 1 ]] && echo dry || echo live)" "$2" \
        >> "$logfile"
}
last_line() { printf '%s\n' "$1" | tail -n 1; }

# Clean whisper text for r2-say.sh: drop [..] and (..) markers such as [BLANK_AUDIO],
# keep only letters, digits, spaces and . , ! ? ' -, start with a letter or digit
clean() {
    printf '%s' "$1" | sed -e 's/\[[^]]*\]//g' -e 's/([^)]*)//g' -e "s/’/'/g; s/‘/'/g" \
        | tr ':' ' ' | LC_ALL=C tr -cd "A-Za-z0-9 .,!?'-" | tr -s ' ' | sed -e 's/^[^A-Za-z0-9]*//' -e 's/ *$//'
}
cut_words() {  # cut_words <text> <max chars> -> whole words only
    local t="$1" n="$2"
    if (( ${#t} > n )); then
        t="${t:0:n}"
        [[ "$t" == *" "* ]] && t="${t% *}"
    fi
    printf '%s' "$t"
}

# The robot answers /say when playback starts, and it does not refuse head moves while
# audio plays; powering the servos then cuts the speech short. Wait for the clip to end.
wait_clip() {  # wait_clip <r2-say output>; clip is 16 kHz mono 16-bit after a 44-byte header
    local bytes
    bytes="$(field "$(printf '%s\n' "$1" | head -n 1)" bytes)"
    [[ "$bytes" =~ ^[0-9]+$ ]] || bytes=196652  # unknown: assume the longest clip (about 6 s)
    sleep "$(awk -v b="$bytes" 'BEGIN { printf "%.2f", (b - 44) / 32000 + 0.5 }')"
}
speak() {  # speak <text>; 0 if the robot played it
    local out
    out="$("$say" "$1" 2>&1)" || { echo "$(now)   speech not made: $(last_line "$out")"; return 1; }
    if [[ "$(last_line "$out")" != "HTTP 200" ]]; then
        echo "$(now)   robot did not speak: $(printf '%s\n' "$out" | head -n 1) ($(last_line "$out"))"
        return 1
    fi
    wait_clip "$out"
}
speak_heard() {  # speak_heard <clean text>; shorter cuts if the clip is too long
    local n
    for n in 71 45 25; do
        local t; t="$(cut_words "$1" "$n")"
        [[ -n "$t" ]] || return 1
        local out
        if out="$("$say" "$prefix$t" 2>&1)"; then
            if [[ "$(last_line "$out")" == "HTTP 200" ]]; then
                wait_clip "$out"
                return 0
            fi
            echo "$(now)   robot did not speak: $(printf '%s\n' "$out" | head -n 1) ($(last_line "$out"))"
            return 1
        fi
        [[ "$out" == *"audio is"*"limit"* ]] || { echo "$(now)   speech not made: $(last_line "$out")"; return 1; }
        echo "$(now)   reply too long to speak; trying a shorter cut"
    done
    return 1
}
move_head() {  # move_head <yaw> <pitch> (whole degrees); one retry if in cooldown or busy
    local out attempt
    for attempt in 1 2; do
        out="$("$head" move "$1" "$2" 2>&1)" || true
        if [[ "$(last_line "$out")" == "HTTP 200" && "$out" == *'"moved":true'* ]]; then
            return 0
        fi
        if [[ $attempt == 1 && "$(last_line "$out")" =~ ^HTTP\ (429|409)$ ]]; then
            sleep "$move_cooldown"
            continue
        fi
        move_err="$(printf '%s\n' "$out" | head -n 1) ($(last_line "$out"))"
        return 1
    done
}
recentre() {  # move the head back to centre; via a halfway point if the step is over 30°
    local st yaw pitch
    move_err=""
    move_head "$centre_yaw" "$centre_pitch" && return 0
    if [[ "$move_err" == *"step too large"* ]]; then
        # Several tap tilts can add up to 40° from centre; go halfway, then to centre
        st="$(robot GET /status 2>/dev/null)" || st=""
        yaw="$(field "$st" last_yaw)"; pitch="$(field "$st" last_pitch)"
        if [[ "$yaw" =~ ^-?[0-9]+$ && "$pitch" =~ ^-?[0-9]+$ ]]; then
            sleep "$move_cooldown"
            move_head $(( (yaw / 10 + centre_yaw) / 2 )) $(( (pitch / 10 + centre_pitch) / 2 )) &&
                sleep "$move_cooldown" && move_head "$centre_yaw" "$centre_pitch" && return 0
        fi
    fi
    echo "$(now)   head not recentred: $move_err"
    return 1
}

# Clear a tap that is already pending (an old tap), ignoring it
drop_pending() {  # drop_pending <reason>
    local ev seq
    ev="$(robot GET /events 2>/dev/null)" || return 0
    if [[ "$(field "$ev" tap_pending)" == "true" ]]; then
        seq="$(field "$ev" tap_seq)"
        if [[ "$seq" =~ ^[0-9]+$ ]]; then
            robot POST "/events/ack?seq=$seq" >/dev/null 2>&1 || true
            echo "$(now) tap #$seq ignored ($1)"
            log "$seq" "ignored-$1"
        fi
    fi
}

handle_tap() {  # handle_tap <tap#>
    local seq="$1" out text spoken
    if (( dry )); then
        echo "$(now)   dry run: would record ${rec_secs} s, transcribe locally, speak it, recentre"
        if speak "I felt a tap on my head."; then
            recentre && log "$seq" "spoken-recentred" || log "$seq" "spoken-not-recentred"
        else
            log "$seq" "not-spoken"
        fi
        return
    fi

    echo "$(now)   recording ${rec_secs} s: speak after the beep"
    if ! out="$("$hear" "$rec_secs" 2>&1)"; then
        echo "$(now)   no recording: $(printf '%s\n' "$out" | grep -m1 -E 'error|HTTP' || last_line "$out")"
        log "$seq" "no-recording"
        recentre || true
        return
    fi
    text="$(clean "$(last_line "$out")")"
    if [[ -z "$text" || "$(last_line "$out")" == "(no speech recognised)" ]]; then
        echo "$(now)   heard: (nothing recognised)"
        speak "Sorry, I did not catch that." && spoken=sorry || spoken=not-spoken
    else
        echo "$(now)   heard: $text"
        speak_heard "$text" && spoken=spoken || spoken=not-spoken
    fi
    if recentre; then log "$seq" "$spoken-recentred"; else log "$seq" "$spoken-not-recentred"; fi
}

trap 'echo; echo "stopped ($(now))"; exit 0' INT TERM
echo "watching http://$ip:$port for head taps ($([[ $dry == 1 ]] && echo 'DRY RUN, no recording' || echo "live, ${rec_secs} s recordings")); Ctrl-C to stop"
drop_pending "pending at start"

replies=()   # epoch seconds of replies in the last hour
last_quiet=""
fails=0
while true; do
    if ev="$(robot GET /events 2>/dev/null)" && [[ "$ev" == *tap_pending* ]]; then
        if (( fails >= 3 )); then
            echo "$(now) robot reachable again"
        fi
        fails=0
        quiet="$(field "$ev" quiet)"
        if [[ "$quiet" != "$last_quiet" ]]; then
            [[ "$quiet" == "true" ]] && echo "$(now) robot is in QUIET mode (taps are ignored by the robot)"
            [[ "$quiet" == "false" && -n "$last_quiet" ]] && echo "$(now) robot left QUIET mode"
            last_quiet="$quiet"
        fi
        if [[ "$(field "$ev" tap_pending)" == "true" ]]; then
            seq="$(field "$ev" tap_seq)"
            if [[ "$seq" =~ ^[0-9]+$ ]]; then
                robot POST "/events/ack?seq=$seq" >/dev/null 2>&1 || true
                t_now=$(date +%s)
                recent=()
                for t in ${replies[@]+"${replies[@]}"}; do
                    (( t_now - t < 3600 )) && recent+=("$t")
                done
                replies=(${recent[@]+"${recent[@]}"})
                if (( ${#replies[@]} >= max_per_hour )); then
                    echo "$(now) tap #$seq ignored (limit of $max_per_hour replies an hour reached)"
                    log "$seq" "ignored-hourly-limit"
                else
                    echo "$(now) tap #$seq"
                    replies+=("$t_now")
                    handle_tap "$seq"
                    drop_pending "arrived during the reply"
                fi
            fi
        fi
    else
        fails=$(( fails + 1 ))
        if (( fails == 3 )); then
            echo "$(now) robot not responding; still trying"
        fi
    fi
    sleep "$poll_secs"
done
