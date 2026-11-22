#!/bin/sh
# Runs the browser checks in parallel, each script against a server of its own (so the
# session cap and the persistent sessions of one script cannot disturb another).
#   run-e2e-parallel.sh path/to/matterm-server path/to/build-web/site [outdir] [jobs]
# Env: MATTERM_DAEMON (path to matterm-daemon; e2e-local.js is skipped without it),
#      NODE_PATH (playwright-core). Exit status is non-zero if any script failed.
# run-e2e.sh stays as the sequential reference.
set -eu
if [ "${1:-}" = "--one" ]; then
    # worker: --one server site out password cred persistent_json script kind port
    shift
    server=$1 site=$2 out=$3 password=$4 cred=$5 pjson=$6 script=$7 kind=$8 port=$9
    here=$(cd "$(dirname "$0")" && pwd)
    work=$(mktemp -d)
    spid=; dpid=
    trap 'kill $spid $dpid 2>/dev/null || true; rm -rf "$work"' EXIT
    url="http://localhost:$port/"
    start() { # extra server args...
        "$server" --credentials "$cred" --port "$port" --origin "http://localhost:$port" \
            --static-dir "$site" "$@" -- /bin/bash >"$work/server.log" 2>&1 &
        spid=$!
        sleep 1
    }
    case $kind in
    plain) start ;;
    persistent) start --config "$pjson" ;;
    local)
        daemon=${MATTERM_DAEMON:-$(dirname "$server")/matterm-daemon}
        [ -x "$daemon" ] || { echo "SKIP $script: no matterm-daemon (set MATTERM_DAEMON)"; exit 0; }
        sock="$work/daemon.sock"
        SHELL=/bin/bash "$daemon" --daemonize --daemon-socket "$sock" --idle-seconds 0
        dpid=$(pgrep -f -- "--daemon-socket $sock" | head -n 1 || true)
        start --config "$pjson" --expose-local-sessions --daemon-socket "$sock"
        ;;
    none) ;;
    esac
    shot="$out/$script"
    mkdir -p "$shot"
    case $script in
    e2e-touch) set -- "$url" "$shot/touch.png" "$password" ;;
    e2e-gestures) set -- "$url" "$password" "$shot/gestures.png" ;;
    e2e-copy | e2e-select | e2e-link) set -- "$url" "$password" ;;
    e2e-local) set -- "$url" "$password" "$shot" "$sock" ;;
    e2e-demo) set -- "${url}?demo" "$shot/demo.png" ;;
    *) set -- "$url" "$password" "$shot" ;;
    esac
    if node "$here/$script.js" "$@" >"$shot/log.txt" 2>&1; then
        echo "ok   $script"
    else
        echo "FAIL $script (log: $shot/log.txt)"
        exit 1
    fi
    # the local daemon exits by itself (idle 0) once the server is gone
    exit 0
fi

server=$1 site=$2 out=${3:-$(mktemp -d)} jobs=${4:-8}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
password='end to end test passphrase 0123456789'
printf '%s\n' "$password" | "$server" --set-password --credentials "$work/cred"
cat >"$work/persistent.json" <<'JSON'
{"persistent_sessions": true, "replay_buffer_bytes": 262144, "replay_total_bytes": 4194304}
JSON
base=$((20000 + $$ % 20000))
# script kind (plain: default server, persistent: keeps terminals, local: daemon + sharing)
cat >"$work/jobs.txt" <<'EOF'
e2e-input plain
e2e-copy plain
e2e-select plain
e2e-link plain
e2e-lifecycle plain
e2e-touch plain
e2e-gestures plain
e2e-shortcuts plain
e2e-pulse plain
e2e-frame plain
e2e-tabs persistent
e2e-tabs-layout persistent
e2e-tabs-touch persistent
e2e-tabs-palette persistent
e2e-copymode persistent
e2e-tabs-limit persistent
e2e-local local
e2e-demo plain
EOF
i=0
while read -r script kind; do
    echo "$script $kind $((base + i))"
    i=$((i + 1))
done <"$work/jobs.txt" >"$work/jobs2.txt"
status=0
xargs -P "$jobs" -L 1 sh "$here/run-e2e-parallel.sh" --one "$server" "$site" "$out" \
    "$password" "$work/cred" "$work/persistent.json" <"$work/jobs2.txt" || status=$?
echo "screenshots and logs in $out"
exit $status
