#!/bin/sh
# Starts matterm-server with a throwaway password on a free port and runs the
# browser checks against it. Needs playwright-core (NODE_PATH or npm install).
#   run-e2e.sh path/to/matterm-server path/to/build-web/site [outdir]
set -eu
server=$1; site=$2; out=${3:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)
pid2=
work=$(mktemp -d); trap 'kill $pid $pid2 2>/dev/null; rm -rf "$work"' EXIT
password='end to end test passphrase 0123456789'
printf '%s\n' "$password" | "$server" --set-password --credentials "$work/cred"
port=$((20000 + $$ % 20000))
"$server" --credentials "$work/cred" --port "$port" \
    --origin "http://localhost:$port" --static-dir "$site" -- /bin/bash \
    >"$work/server.log" 2>&1 &
pid=$!
sleep 1
url="http://localhost:$port/"
node "$here/e2e-input.js" "$url" "$password" "$out"
node "$here/e2e-copy.js" "$url" "$password"
node "$here/e2e-select.js" "$url" "$password"
node "$here/e2e-link.js" "$url" "$password"
node "$here/e2e-lifecycle.js" "$url" "$password" "$out"
node "$here/e2e-touch.js" "$url" "$out/touch.png" "$password"
node "$here/e2e-gestures.js" "$url" "$password" "$out/gestures.png"
node "$here/e2e-frame.js" "$url" "$password" "$out"
# Tabs and panes: a second server that keeps its terminals (persistent
# sessions), so that a reload can attach to them again.
cat >"$work/persistent.json" <<'JSON'
{"persistent_sessions": true, "replay_buffer_bytes": 262144, "replay_total_bytes": 4194304}
JSON
port2=$((port + 1))
"$server" --config "$work/persistent.json" --credentials "$work/cred" \
    --port "$port2" --origin "http://localhost:$port2" --static-dir "$site" \
    -- /bin/bash >"$work/server2.log" 2>&1 &
pid2=$!
sleep 1
url2="http://localhost:$port2/"
node "$here/e2e-tabs.js" "$url2" "$password" "$out"
node "$here/e2e-tabs-layout.js" "$url2" "$password" "$out"
node "$here/e2e-demo.js" "${url}?demo" "$out/demo.png"
echo "screenshots in $out"
