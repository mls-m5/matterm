# Security review of the sharing path (daemon, exposed connections, server bridge)

Reviewed 2026-11-22 (agent2), code at main a6080e7. Tests first: every finding below has a test in
`test/daemonreview_test.cpp` (the test failed before the fix), the earlier behaviour is covered by
`test/daemon_test.cpp` and `test/localsessions_test.cpp`. Threat model: another local user of the
machine; a browser with the right password (or a stolen cookie); a compromised web page driving
the server; a crash at a bad moment. Any process of the *owner* is trusted by design (it can
already run anything), so "same uid" is the boundary of the daemon.

## Findings, fixed

| # | Severity | Finding | Fix |
| --- | --- | --- | --- |
| 1 | high (when the socket directory is writable by others, e.g. `--daemon-socket /tmp/x.sock`) | The lock file `<socket>.lock` was opened with `O_CREAT` and **followed symlinks**, then truncated and written: a symlink planted there made the daemon overwrite any file of the user's. | `O_NOFOLLOW`, and `fstat` must show a regular file of ours. |
| 2 | medium | A stale socket was removed with a plain `unlink`, whatever it was (a symlink or a regular file placed there got removed). | Only a socket owned by us is replaced; anything else is an error. |
| 3 | medium | The default private directory (`$XDG_RUNTIME_DIR/matterm`, `/tmp/matterm-<uid>`) was checked with `stat`, so a symlink to some other directory of ours was accepted (the socket and lock land in a place the planter chose). | `lstat`: a symlink is refused. Mode 0700 and owner were already checked. |
| 4 | medium | Neither the server nor the GUI client checked who owns the socket they connect to. Under the `/tmp/matterm-<uid>` fallback another user could create the directory and a socket first, and would receive the browser's keystrokes or the GUI's. | Both check `SO_PEERCRED` of the connected socket against their own uid (`DaemonConnection`, `UnixSocketChannel`; `ServerConfig::daemonPeerUid` only for tests). |
| 5 | medium | A server-bridged browser connection whose daemon accepted but never answered was held forever (the Hello deadline did not apply once the bridge existed). | The daemon gets `session.hello_timeout` to answer, then the browser gets `Error Timeout` and the connection closes. |
| 6 | low | An exposed connection that sent nothing after `ExposedHello` kept a client slot for ever (32 of them stop the GUI from connecting; same-uid only, but cheap). | The Hello timeout applies until an exposed connection has attached. Control connections stay open on purpose (the GUI keeps one). |
| 7 | low | After the owner confirmed "Shutdown, ending N sessions", a session created before the daemon exited (another connection, same loop turn) was killed without confirmation. | A confirmed Shutdown sets a flag: `CreateSession` answers Busy and `Hello` answers `ShuttingDown` from then on. |
| 8 | low | The umask was left at 077 when binding failed. | RAII guard. |

## Checked and found sound (tests)

- **Peer check on every path:** it runs in `accept`, before a connection object exists, so control,
  exposed and terminal connections all pass through it (test: `Another user is refused without a word`).
  Refused peers get nothing, not even an error.
- **Modes:** socket 0600 (created under umask 077, then `chmod`), lock 0600, created directory 0700.
- **Two daemons / stale socket / crash:** `flock` on the lock file is released by the kernel when the
  process dies, so a restart after a crash works; a second daemon refuses.
- **ExposedHello restrictions:** list shows shared sessions only; attaching needs an existing shared
  session; no session id (which would create one) is refused; anything else is `ControlError` and a close.
  A private, unknown, malformed and ended session produce **byte-identical** error frames
  (`Private, unknown, malformed and ended...`). The answer is produced on the same code path before the
  registry is consulted, so there is no length difference; the lookup is a linear scan over at most 64
  in-memory entries, which is not observable through a socket, let alone through the web server.
- **Sharing stop cuts the bridge** (`Replaced "sharing stopped"`), the session keeps running and its owner
  can take it back; a second `Hello` sent through a bridge cannot reach another session (frames after the
  first `Hello` only ever go to the registry entry of that attachment; control-type frames are junk to it).
- **Server bridge:** `local:<id>` accepts `[A-Za-z0-9_-]{1,64}` only; every odd form (empty, `..`, spaces,
  NUL, doubled prefix, wrong case, trailing slash) gets the same refusal as an unknown id and attaches nothing.
  `/local-sessions` is POST only, needs the cookie and the exact Origin (wrong or missing Origin, wrong
  cookie, GET, PUT, trailing slash: all refused, no ids in the body). Logging out, or logging in again,
  closes bridges within the 10 ms tick (cookie generation check). Without `--expose-local-sessions` the
  endpoint is 404 and `local:` is refused; the server never opens the daemon socket.
- **Daemon dies or restarts while bridged:** the browser connection closes; a new daemon does not know the
  old ids (refused like any unknown id).
- **Logs:** the server logs `Local session take-over attempt session=<id> source=<keyed hash>`; no screen
  text, no password, no cookie (test captures `std::clog` through a session in which a marker was typed).
  The daemon logs nothing.
- **Limits:** frames 1..16384 (bigger or zero closes), per-connection output queue 512 KiB / 1024 messages,
  `maxClients` 32 (the next is closed without a word), `maxSessions` 64 (`CreateSession` then answers
  `StartFailed`), `max_connections`/`maxUpgradesPerLogin` (16) on the server side bound the browser's use,
  idle exit when there is no session and no client.

## Accepted, documented, not changed

- **Same uid is trusted.** Any process of the owner can list, share, attach and shut down. That is the
  point of an owner-only socket; the sharing flag protects against the *web server* and its clients, not
  against the owner's other programs.
- **A shared terminal is a shell as the user.** The browser can type anything, including `Close` (ends the
  shell). The daemon's shells inherit the allow-listed environment of the daemon, which includes
  `SSH_AUTH_SOCK`: sharing a terminal gives the browser the ssh agent. Remove it from
  `defaultConfig().process.environmentAllowList` if that is not wanted.
- **History becomes visible when a terminal is shared.** The replay ring keeps the last output (up to
  4 MiB) from before the sharing started; a browser attaching from offset 0 sees it. (Unsharing does not
  recall what a browser already received.)
- **Take-over storms** are bounded by the leases (16 attachments per login) but not rate limited per
  second: each take-over makes the shell see a resize (SIGWINCH). It needs the password and only annoys.
- **`/sessions/close-all` and `/logout` do not touch daemon sessions** (they act on the server's own
  registry). A bridged session just detaches.
- Session ids appear in the server log. They are not secrets on their own (the daemon only lets an
  exposed connection attach to shared sessions) but they are unguessable 128-bit values, so keep the log private.
- `ControlHello` connections never time out (the GUI keeps one open).
- Control version 1 daemons cannot be told apart from broken ones by the client other than through
  `ControlWelcome.version`.

## Test support

`matterm-daemon-e2e` (built with the server, not installed) starts a daemon on a temporary socket for browser
end-to-end tests: `matterm-daemon-e2e --socket /tmp/x/d.sock --sessions 2 --share 1 --send 'echo hello'` prints
`{"socket":...,"sessions":[{"id":...,"shared":true,"attached":false},...]}` and then answers `list`, `new`,
`share ID`, `unshare ID` and `quit` (or EOF) on stdin. It refuses a socket outside the temp directory (or
`$MATTERM_E2E_DIR`), so it cannot touch the real daemon. The production `matterm-daemon` has no test options.
