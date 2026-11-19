#!/bin/sh
# Starts matterm-server with a throwaway password on a free port and runs the
# browser checks against it. Needs playwright-core (NODE_PATH or npm install).
#   run-e2e.sh path/to/matterm-server path/to/build-web/site [outdir]
set -eu
server=$1; site=$2; out=${3:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d); trap 'kill $pid 2>/dev/null; rm -rf "$work"' EXIT
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
echo "screenshots in $out"
