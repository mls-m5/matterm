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

## Follow-up: first connection times out after remote installation

Owner report (2026-11-23; record for later investigation, not reproduced here):
`matterm --daemon-host user@lan-host` often fails on the first attempt, while
running the same command again succeeds. The failing attempt prompts to install
this daemon version, reports successful copying of both versioned binaries
(`matterm-daemon` and `matmux`), removes older binaries, then exits with:

```text
--daemon-host: ssh user@lan-host: no answer in time
```

The reported daemon version was `1.3.4-276925e3`; the copied matmux version was
`1.3.4-b419744e`. Installation success is reported before the timeout; this does
not establish whether daemon startup, readiness, socket forwarding or the initial
handshake is delayed. Do not assume a cause or merely raise the timeout.

Investigate install -> daemon start -> remote socket readiness -> SSH forward ->
initial protocol response, including whether installation consumes the startup
deadline and whether the differing binary build identifiers are expected. Add a
first-install regression test with delayed readiness and verify that a successful
installation can connect without a second manual launch. Real LAN/SSH validation
remains required. Keep this separate from the current perflog and settings work.
