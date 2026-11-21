#!/bin/sh
# Starts the Mattrix chat locally for testing and prints where to connect.
#   tools/chat-local.sh [--host ADDR] [--web-port N] [--ws-port N] [--tcp-port N]
#                       [--build DIR] [--site DIR]
# Starts chatd (WebSocket + a loopback-only plain TCP test port) and, if a built web
# client exists, a tiny static web server for it. Stops everything on Ctrl+C; from
# another terminal (or if that did not work) use tools/chat-local-stop.sh.
#   --host ADDR     address for the WebSocket and the web server (default 127.0.0.1).
#                   Anything but a loopback address exposes the chat to your network
#                   WITHOUT TLS; use it only on a network you trust. The plain TCP
#                   test port is always loopback.
#   --web-port N    web server port (default 8080; also CHAT_WEB_PORT)
#   --ws-port N     chatd WebSocket port (default 8097; also CHAT_WS_PORT)
#   --tcp-port N    chatd TCP test port (default 8098; also CHAT_TCP_PORT)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=$root/build; site=$root/build-web/site
host=127.0.0.1
ws=${CHAT_WS_PORT:-8097}; tcp=${CHAT_TCP_PORT:-8098}; web=${CHAT_WEB_PORT:-8080}
while [ $# -gt 0 ]; do
    case "$1" in
        --host) host=${2:?--host needs an address}; shift 2 ;;
        --web-port) web=${2:?--web-port needs a number}; shift 2 ;;
        --ws-port) ws=${2:?--ws-port needs a number}; shift 2 ;;
        --tcp-port) tcp=${2:?--tcp-port needs a number}; shift 2 ;;
        --build) build=${2:?--build needs a directory}; shift 2 ;;
        --site) site=${2:?--site needs a directory}; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
    esac
done
case "$host" in
    127.*|localhost|::1) ;;
    *) echo "WARNING: $host is not a loopback address: the chat (no TLS) will be reachable from your network." >&2 ;;
esac
state=${XDG_RUNTIME_DIR:-/tmp}/matterm-chat-local-$(id -u)
chatd=$build/chatd; [ -x "$chatd" ] || { echo "no $chatd; build first: cmake -S . -B build && cmake --build build" >&2; exit 1; }
if [ -e "$state/pids" ]; then
    echo "already running (or not stopped): run tools/chat-local-stop.sh first" >&2; exit 1
fi
mkdir -p "$state"; chmod 700 "$state"; : > "$state/pids"
cleanup() {
    trap - INT TERM EXIT
    [ -f "$state/pids" ] && while read -r pid; do kill "$pid" 2>/dev/null || true; done < "$state/pids"
    rm -rf "$state"
    echo; echo "chat stopped."
}
trap cleanup INT TERM EXIT
"$chatd" --origin "http://$host:$web" --origin "http://localhost:$web" --origin "http://127.0.0.1:$web" \
    --listen "$host:$ws" --test-tcp "127.0.0.1:$tcp" >"$state/chatd.log" 2>&1 &
echo $! >> "$state/pids"
have_web=no
if [ -f "$site/demo.html" ] && command -v python3 >/dev/null 2>&1; then
    mkdir -p "$state/site"; cp -r "$site"/. "$state/site"/; cp "$site/demo.html" "$state/site/index.html"
    (cd "$state/site" && exec python3 -m http.server "$web" --bind "$host" >"$state/web.log" 2>&1) &
    echo $! >> "$state/pids"; have_web=yes
fi
sleep 1
if ! kill -0 "$(head -n 1 "$state/pids")" 2>/dev/null; then
    echo "chatd did not start:" >&2; cat "$state/chatd.log" >&2; exit 1
fi
case "$host" in
    127.*|localhost|::1) echo "Mattrix chat is running (local test only, nothing leaves this machine)." ;;
    *) echo "Mattrix chat is running on $host (reachable from your network, no TLS)." ;;
esac
echo
echo "  In a terminal (two or more, with different names):"
echo "    $build/mattrix-chat --tcp 127.0.0.1:$tcp --name ada"
echo "    $build/mattrix-chat --tcp 127.0.0.1:$tcp --name bo"
if [ "$have_web" = yes ]; then
    echo
    echo "  In a browser (several tabs work):"
    echo "    http://$host:$web/?chat=ws://$host:$ws"
    echo "    type 'chat' in the pretend system and answer y"
else
    echo
    echo "  (No web client: $site/demo.html missing or no python3, so no browser URL.)"
fi
echo
echo "  chatd log: $state/chatd.log"
echo "  Stop with Ctrl+C here, or tools/chat-local-stop.sh from another terminal."
wait
