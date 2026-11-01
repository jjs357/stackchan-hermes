#!/bin/bash
# r7-view.sh — turn the camera view on the robot's own screen on or off (R7 sketch).
# Frames are shown only on the robot; nothing is sent to this Mac.
#
# Usage: ./r7-view.sh on | off [--no-token]
#   --no-token   send the request without the token (test: the robot must refuse with 401)
# Prints only the robot's JSON reply and the HTTP status code. Never prints the token.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r7.conf"

case "${1:-}" in
    on) on=1 ;;
    off) on=0 ;;
    *) echo "usage: $0 on|off [--no-token]" >&2; exit 2 ;;
esac
send_token=1
if [[ "${2:-}" == "--no-token" ]]; then
    send_token=0
elif [[ $# -gt 1 ]]; then
    echo "usage: $0 on|off [--no-token]" >&2
    exit 2
fi

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"
if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r7.conf" >&2
    exit 2
fi

url="http://$ip:$port/camera/view?on=$on"
if (( send_token == 0 )); then
    curl -sS --max-time 15 -X POST -w '\nHTTP %{http_code}\n' "$url"
    exit 0
fi

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
curl -sS --max-time 15 -X POST \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -w '\nHTTP %{http_code}\n' \
    "$url"
