#!/bin/bash
# r3-head.sh — read or move the StackChan head (R3 sketch).
# Uses the token in the macOS Keychain; never prints it.
#
# Usage:
#   ./r3-head.sh read                     power servos briefly, report angles (no movement)
#   ./r3-head.sh move <yaw> <pitch>       one slow move; whole degrees
#                                         yaw -30..30, pitch 10..70, max 30° change per axis
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r3.conf"

usage() {
    echo "usage: $0 read | move <yaw -30..30> <pitch 10..70>" >&2
    exit 2
}

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r3.conf" >&2
    exit 2
fi

case "${1:-}" in
    read)
        [[ $# -eq 1 ]] || usage
        path="/head/read"
        ;;
    move)
        # Plain integers without leading zeros (bash would read "08" as octal)
        [[ $# -eq 3 && "$2" =~ ^-?(0|[1-9][0-9]?)$ && "$3" =~ ^(0|[1-9][0-9]?)$ ]] || usage
        yaw=$2
        pitch=$3
        # Same limits as the sketch; the robot re-checks everything
        if (( yaw < -30 || yaw > 30 || pitch < 10 || pitch > 70 )); then
            echo "error: yaw must be -30..30 and pitch 10..70 (degrees)" >&2
            exit 2
        fi
        path="/head/move?yaw=$((yaw * 10))&pitch=$((pitch * 10))"
        ;;
    *)
        usage
        ;;
esac

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
curl -sS --max-time 15 -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -w '\nHTTP %{http_code}\n' \
    "http://$ip:$port$path"
