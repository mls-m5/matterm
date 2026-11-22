# Reconnect for `--daemon-host` (agent2)

Goal: when the ssh tunnel to a remote daemon dies (network drop, laptop sleep), the window
restarts the tunnel and reattaches its panes instead of closing them.

Scope
- Detect a dead ssh process or socket; restart the `ssh -L` forward with the same options;
  `DaemonLink` resumes each pane by replay offset (it already does this after a lost
  local connection).
- Palette entry "Reconnect" (manual), plus automatic retries with backoff (cap, then a
  visible pane state "disconnected, Enter to retry").
- No blocking calls on the UI thread; the 20 s start-up wait must not freeze the window.
- Tests against `test/fakessh.py` (kill the fake ssh, expect reconnect and no lost bytes).
- Update `docs/REMOTE_DAEMON.md`.

Out of scope: native WebSocket client, new transports, protocol changes.

Owned paths: `src/frontend/sshtunnel.h`, `src/frontend/daemonlink.h`, `test/sshtunnel_test.cpp`,
`test/fakessh.py`, `docs/REMOTE_DAEMON.md`. Shared (report edits, keep them minimal):
`app/ptyfrontend.cpp`, palette command registration (agent3 owns `commandregistry`).

Acceptance: ctest green, new tests fail before and pass after, tree warning-free,
clang-format run. State plainly that real sshd was not available.
