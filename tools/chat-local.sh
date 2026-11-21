#!/bin/sh
# Starts the Mattrix chat locally for testing and prints where to connect.
#   tools/chat-local.sh [build-dir] [web-site-dir]
# Starts chatd (WebSocket + a loopback-only plain TCP test port) and, if a built web
# client exists, a tiny static web server for it. Stops everything on Ctrl+C; from
# another terminal (or if that did not work) use tools/chat-local-stop.sh.
# Ports can be changed: CHAT_WS_PORT (8097), CHAT_TCP_PORT (8098), CHAT_WEB_PORT (8080).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
site=${2:-$root/build-web/site}
ws=${CHAT_WS_PORT:-8097}; tcp=${CHAT_TCP_PORT:-8098}; web=${CHAT_WEB_PORT:-8080}
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
"$chatd" --origin "http://127.0.0.1:$web" --origin "http://localhost:$web" \
    --listen "127.0.0.1:$ws" --test-tcp "127.0.0.1:$tcp" >"$state/chatd.log" 2>&1 &
echo $! >> "$state/pids"
have_web=no
if [ -f "$site/demo.html" ] && command -v python3 >/dev/null 2>&1; then
    mkdir -p "$state/site"; cp -r "$site"/. "$state/site"/; cp "$site/demo.html" "$state/site/index.html"
    (cd "$state/site" && exec python3 -m http.server "$web" --bind 127.0.0.1 >"$state/web.log" 2>&1) &
    echo $! >> "$state/pids"; have_web=yes
fi
sleep 1
if ! kill -0 "$(head -n 1 "$state/pids")" 2>/dev/null; then
    echo "chatd did not start:" >&2; cat "$state/chatd.log" >&2; exit 1
fi
echo "Mattrix chat is running (local test only, nothing leaves this machine)."
echo
echo "  In a terminal (two or more, with different names):"
echo "    $build/mattrix-chat --tcp 127.0.0.1:$tcp --name ada"
echo "    $build/mattrix-chat --tcp 127.0.0.1:$tcp --name bo"
if [ "$have_web" = yes ]; then
    echo
    echo "  In a browser (several tabs work):"
    echo "    http://127.0.0.1:$web/?chat=ws://127.0.0.1:$ws"
    echo "    type 'chat' in the pretend system and answer y"
else
    echo
    echo "  (No web client: $site/demo.html missing or no python3, so no browser URL.)"
fi
echo
echo "  chatd log: $state/chatd.log"
echo "  Stop with Ctrl+C here, or tools/chat-local-stop.sh from another terminal."
wait
