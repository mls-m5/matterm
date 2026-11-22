# Local daemon: workstreams (docs/TABS_PLAN.md sections 10 and 11)

Read sections 10 and 11 of docs/TABS_PLAN.md first. Decisions of the owner: the daemon
must change rarely (GUI changes must not force a restart), it owns PTYs and byte rings
only, it is stopped from the palette, terminals are private unless marked shared, and the
browser may later take shared terminals over via `matterm-server --expose-local-sessions`.

Framing on the Unix socket (decided here so both sides can start): each message is a
4-byte big-endian length (1..16384) followed by the same bytes a WebSocket binary message
carries today (docs/PROTOCOL.md). Anything else closes the connection.

## A. The daemon (agent2) -- `src/daemon/`, executable `matterm-daemon`
- Reuse `SessionRegistry` and the PTY session host of the server; no network, no password:
  Unix socket in `$XDG_RUNTIME_DIR/matterm/daemon.sock` (directory 0700, socket 0600, fall
  back to a 0700 directory under /tmp named with the uid), peer uid checked with
  `SO_PEERCRED`, refuse other users.
- `--daemon-socket PATH` for a development daemon. Starts on demand (a client may spawn it
  with `--daemonize`), exits by itself after N seconds with no sessions and no client
  (default 60). A pid/lock file prevents two daemons on one socket.
- Protocol: additive only inside a major version; the Hello/Welcome feature list gains
  `daemon` (control messages: list sessions, mark shared / not shared, status, shutdown).
  Shutdown must say how many sessions end, and only runs when the client confirmed.
- Tests: framing (fuzz: partial, oversize, zero length), peer uid refusal where testable,
  sessions survive a client disconnect, replay after reconnect, idle exit, two daemons,
  shutdown counts.

## B. The client side (agent3) -- `src/transport/unixchannel.*`, `src/frontend/daemonlink.h`, native wiring
- `UnixSocketChannel` implementing `MessageChannel` with the framing above (non-blocking,
  size limits), usable by `ClientSession` exactly as the WebSocket channel is.
- `DaemonLink` (TerminalLink over ClientSession) so that a native pane can use the daemon
  instead of an in-process `PtyLink`; `matterm --daemon` (default off at first) and
  `--no-daemon`; start the daemon when missing; palette actions "Stop daemon" (confirm, with
  the number of programs that would end), "Quit and keep running", "Daemon status".
- Pane states for a lost connection and for a take-over by another window, reusing the
  `TerminalView` states; Enter attaches again from the replay offset.
- Wire it only after A's socket and framing exist; until then test with a fake channel.

## C. Sharing (later, after A and B)
`matterm-server --expose-local-sessions`, the shared mark per session in the daemon, the
strip button and palette entries, read-only attach.
