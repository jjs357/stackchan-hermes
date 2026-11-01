#!/bin/bash
# r1-status.sh — read StackChan R1 status (GET /status) using the token in the macOS Keychain.
# Prints only the robot's JSON reply and the HTTP status code. Never prints the token.
# Read-only: the R1 sketch has no command endpoints.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
conf="$here/r1.conf"

# Parse settings instead of sourcing them, so the file cannot run code
ip="$(sed -n 's/^ROBOT_IP=//p' "$conf")"
port="$(sed -n 's/^ROBOT_PORT=//p' "$conf")"

if ! [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ && "$port" =~ ^[0-9]{1,5}$ ]]; then
    echo "error: ROBOT_IP/ROBOT_PORT missing or invalid in r1.conf" >&2
    exit 2
fi

if ! token="$(security find-generic-password -s stackchan-r1-token -w 2>/dev/null)"; then
    echo "error: Keychain entry 'stackchan-r1-token' not found" >&2
    exit 2
fi

# printf is a shell builtin and the header is passed through a file descriptor,
# so the token never appears in any process's argument list
curl -sS --max-time 5 \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    -w '\nHTTP %{http_code}\n' \
    "http://$ip:$port/status"
