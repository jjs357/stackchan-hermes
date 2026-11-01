#!/bin/bash
# r6-taps.sh — watch the StackChan (R6 sketch) for head taps and report them. R6a: no AI,
# nothing is recorded; each tap is printed and acknowledged so it is reported once.
#
# Usage:
#   ./r6-taps.sh        runs until you press Ctrl-C (it is never started automatically)
#
# Polls GET /events once a second. Uses the token in the macOS Keychain; never prints it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r6.conf"
poll_secs=1

[[ $# -eq 0 ]] || { echo "usage: $0" >&2; exit 2; }

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r6.conf" >&2
    exit 2
fi
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

trap 'echo; echo "stopped ($(date +%H:%M:%S))"; exit 0' INT TERM
echo "watching http://$ip:$port for head taps (Ctrl-C to stop)"

last_quiet=""
fails=0
while true; do
    if ev="$(robot GET /events 2>/dev/null)" && [[ "$ev" == *tap_pending* ]]; then
        if (( fails > 0 )); then
            echo "$(date +%H:%M:%S) robot reachable again"
        fi
        fails=0
        quiet="$(field "$ev" quiet)"
        if [[ "$quiet" != "$last_quiet" ]]; then
            [[ "$quiet" == "true" ]] && echo "$(date +%H:%M:%S) robot is in QUIET mode"
            [[ "$quiet" == "false" && -n "$last_quiet" ]] && echo "$(date +%H:%M:%S) robot left QUIET mode"
            last_quiet="$quiet"
        fi
        if [[ "$(field "$ev" tap_pending)" == "true" ]]; then
            seq="$(field "$ev" tap_seq)"
            if [[ "$seq" =~ ^[0-9]+$ ]]; then
                ack="$(robot POST "/events/ack?seq=$seq" 2>/dev/null || true)"
                echo "$(date +%H:%M:%S) tap #$seq (age $(field "$ev" tap_age_ms) ms, ack cleared=$(field "$ack" cleared))"
            fi
        fi
    else
        fails=$(( fails + 1 ))
        if (( fails == 3 )); then
            echo "$(date +%H:%M:%S) robot not responding; still trying"
        fi
    fi
    sleep "$poll_secs"
done
