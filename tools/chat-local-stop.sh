#!/bin/sh
# Stops what tools/chat-local.sh started (uses its pid file, never pkill -f).
set -eu
state=${XDG_RUNTIME_DIR:-/tmp}/matterm-chat-local-$(id -u)
if [ ! -f "$state/pids" ]; then
    echo "nothing to stop (no $state/pids)"; exit 0
fi
while read -r pid; do
    kill "$pid" 2>/dev/null && echo "stopped $pid" || echo "$pid was not running"
done < "$state/pids"
rm -rf "$state"
