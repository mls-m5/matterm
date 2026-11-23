# Handover: agent2 (`ln2ow`, branch `agent2-daemon`, worktree `.tmp/agent2`)

Written 2026-11-22 for whoever continues this work. Read `docs/handover/integrator.md` first (project, roles,
verification recipe, rules). The owner speaks Swedish (answer him in Swedish); the docs and code are English.

## What this role owns

- **The daemon** (`src/daemon/`, `docs/PROTOCOL.md` section 12, `docs/tasks/daemon.md`): `matterm-daemon`
  keeps PTY sessions alive on a private Unix socket (4-byte length framing, `framing.h`, control messages in
  `control.h`), the native app is its client (`src/frontend/daemonclient.h`, `daemonlink.h`). Options:
  `--daemon-socket`, `--idle-seconds`, `--daemonize`, `--ensure`, `--print-socket`. Review: `docs/DAEMON_REVIEW.md`.
- **The server side of sharing** (`src/server/`, `localsessions.cpp`, `docs/SERVER_CONFIG.md`):
  `matterm-server --expose-local-sessions` lets a browser attach to daemon sessions the owner marked shared.
  Security review: `docs/SECURITY_REVIEW.md`.
- **Remote daemon over ssh** (`docs/REMOTE_DAEMON.md`, `src/frontend/sshtunnel.h`, `sha256.h`,
  `test/sshtunnel_test.cpp`, `test/fakessh.py`): `matterm --daemon-host [user@]host [--daemon-install] [--yes]
  [--ssh-arg A]`. `ssh -L local:remote` Unix-socket forward, daemon started there with `--ensure`, our own
  static daemon copied (content-named, checked, atomic) when none is on the remote PATH.
- **Reflow** (`src/terminal/reflow.h`, `terminalmodel.cpp`, `docs/SCROLLING_AND_REFLOW.md`, `test/terminalreflow_test.cpp`):
  saved rows and, by default, the rows above the cursor's line are laid out again on a width change;
  lazy slices (newest 1000 rows at once, older ones 1000 per frame once the size was quiet 0.1 s).
- **CPU / render loop** (`app/ptyfrontend.cpp`, `src/frontend/framesignature.h`, `docs/CPU.md`): the native loop
  draws only when a frame signature changed and sleeps otherwise.
- **Reviews and benches**: `bench/modelbench.cpp` (write path and drag cost), `bench/reflowbench.cpp`,
  `bench/claudelike.py` + `bench/cpu-measure.sh` (CPU of the window under a Claude-Code-like load).

## How I worked

- Own worktree and branch; never `cd` to the repo root. First `git merge main` into `agent2-daemon`, then work,
  commit, report the SHA. The integrator merges the branch into `main`. No pushes (the owner pushes).
  Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. `TO_INTEGRATOR.md` and
  `FROM_INTEGRATOR.md` are never committed.
- Tests first for findings: write the failing test, then fix. Fix only trivial things in code owned by others.
- Build dirs: `build/` (debug-ish, used for ctest), a Release dir in the scratchpad for benchmarks
  (`-DCMAKE_BUILD_TYPE=Release -DMATTERM_BUILD_BENCHMARKS=ON`). Static daemon: `-DMATTERM_STATIC_DAEMON=ON`.
- Run one test binary directly (`build/test/terminalreflow_test`) while iterating; `ctest -j4` for the lot.
  After adding files run `make` before `fakeos` (it counts repository lines and is stale until rebuilt).
- Headless native window: `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software build/matterm --no-daemon -- CMD`;
  `MATTERM_LOOP_STATS=1` prints loop rounds and frames drawn at exit.
- Experiment with real programs: record bash on a pty (python `pty.fork`), replay the bytes into `TerminalModel`
  with `VtParser` (that is how the reflow default was chosen; see the "shell problem" in SCROLLING_AND_REFLOW.md).

## What I could not run

- **Real `ssh`/`sshd`** (none installed here): the whole ssh chain is tested against `test/fakessh.py`, which relays
  Unix sockets and runs the "remote" command with `/bin/sh` and a fake HOME. The ssh options used are the documented
  OpenSSH ones (`-L socket:socket`, `StreamLocalBindUnlink`, `ExitOnForwardFailure`, `BatchMode`) but have never
  met a real server. First thing to do on a machine with sshd: `matterm --daemon-host localhost` and
  `--daemon-install --yes` against a second account or a container.
- **A real display / GPU**: all CPU numbers are headless with the software renderer; idle on X11/Wayland should be
  lower (SDL's dummy driver polls every ms) but was not measured. No GUI interaction was ever driven by me.
- A different architecture or glibc for the copied daemon (only the arch-mismatch error path is tested).

## Known gaps and unverified points

- **Static daemon**: builds and passes `sshtunnel_test` (`MATTERM_TEST_DAEMON=path`), but it is not the default
  build; the normal dev daemon is dynamic and is copied with a warning. `-static` glibc is fine only because the
  daemon does no name lookups (no `getpwnam`, no DNS): re-check if that changes. Release builds must set the option.
- **ssh tunnel**: no reconnect (panes close, terminals live on remotely and reappear on the next start); start-up
  blocks up to 20 s (copy: 180 s); blocking `askDaemon` calls on the UI thread (poll every 10 s over a tunnel) stutter on
  slow links; one host per window; `BatchMode=yes` means an unknown host key fails (run `ssh host` once);
  a running remote daemon of an older, incompatible version is reused (never restarted); old `matterm-daemon-*`
  copies are never pruned; remote start needs `~/.local/share` writable and not `noexec`.
- **Lazy reflow memory**: while older rows wait, the old screen exists twice (old width and the laid-out part),
  at most one ring capacity extra; segments are dropped when the ring is full or on `bad_alloc`. Only
  `kMaxSegments = 2`; the quiet timer is driven by `advanceViewport` (every frontend must call it every frame).
- **Reflow modes**: default `AboveCursor` (cursor's logical line and rows below are clipped: bash/readline redraw
  assumes no reflow, `All` leaves a copy of the prompt). DCH/EL/ECH to the right edge clear the wrap flag (line
  ends there). The alternate screen is always clipped. Full-screen programs on the primary screen are not special-cased.
- **Render loop**: output of a program is picked up within 4 ms (active) or 100 ms (idle, after 500 ms quiet), not
  instantly: the loop does not wait on the PTY fds (an SDL user event from a reader thread would). Blink stops after 10 s.
  Anything that animates must change the cells at a low rate (docs/CPU.md); any change keeps the loop awake.
- **Flaky under load** (not mine, but you will meet them in `ctest -j`): `chatd_soak`, `fakeos` (stale line count, see
  above), occasionally `pane_state`. Run them alone before suspecting a change. My own daemon/localsessions fixtures use
  generous waits (`kPatience`, 30 s read deadlines).
- `docs/DAEMON_REVIEW.md` and `docs/SECURITY_REVIEW.md` list review findings already fixed and a few accepted risks
  (same-uid trust model: any process of the owner can use the daemon socket).

## Open ideas, roughly by value

1. **Reconnect** for `--daemon-host`: restart the tunnel when ssh dies and reattach the panes (`DaemonLink` already
   resumes by offset after a lost local connection); a palette entry "Reconnect".
2. **Native WebSocket client** against `matterm-server --expose-local-sessions` (design in REMOTE_DAEMON.md part b):
   a `MessageChannel` implementation, login/cookie/Origin, TLS first; only shared sessions are visible.
3. **Wake on PTY output** (SDL user event from a reader thread) so idle can sleep longer than 100 ms with no latency.
4. `LiveReflow::All` or prompt-aware reflow via shell integration (OSC 133 marks) so the cursor's line can be reflowed
   without duplicating prompts.
5. A glyph lookup per cell is done twice in `SdlBackend::render`; keep the first result (profile in docs/CPU.md).
6. Prune old remote daemon copies; `--remote-daemon PATH`; a version/build id in the daemon `Status` for a clear
   "remote daemon is older" message and an offer to restart it when it has no sessions.

## Reporting to the integrator (n27l7)

- One short message with `mcp__pis-host__send_message` (load it with ToolSearch): branch, SHA, what changed, numbers,
  what was **not** verified, ctest result. Then wait; do not send follow-ups. Long text goes in a doc under `docs/`
  and the message points at it. Never claim something was verified that was only built.
- The integrator merges `main` state back to you via your own `git merge main`; tell it if a merge needed fixes.
- Findings in code you do not own: a failing test or a precise note to the integrator, not a rewrite.

## Status at the pause (2026-11-23, branch `agent2`, everything committed; written for after the pause)

Everything below is merged into main or ready on branch `agent2` (see `git log`), except the two items under "Not started".
Build green; ctest green except that `matmux_pty` needs ~40 s and times out (60 s) when run in parallel with the whole suite
(`ctest -j6`): run it alone. `fakeos` is stale until `make` after adding files.

**Done and tested since the last note** (each has a file in `docs/handover/features/agent2-*.md`): the shared tty layer (`src/tty`, snapshot,
Session), daemon versions / several daemons / `matmux` (renamed from matterm-attach) / list and stop / install names and cleanup,
`CreateTerminal`, `MATTERM_SESSION`, `EndSession`, replay performance (the daemon sent 16 KiB per 10 ms; now the window's worth;
flood drawing at most 10x/s in the window and matmux; `docs/CPU.md`), the passthrough snapshot (`src/tty/matterm/snapshot.*`),
settings (`jsonvalue.h`, `settings.h`, `settingsfile.h`, `settingswatch.h`, `themes.h`, window wiring, `computerNamePlacement`
centre/left/right, name in the strip's gap via `placeComputerName`), and the JSON readers requirements test + table
(`test/jsonreaders_test.cpp`, `agent2-json-readers.md`; jsonvalue passes all 28, sessionsnapshot's reader 17/23, fast-json 15/28).

**Not started** (owner decisions already given in `docs/tasks/settings.md` Addendum 2 and `docs/tasks/json-readers.md` Addendum 2, and `docs/tasks/perf-log.md`):
1. *Settings file formatting*: `Settings::toText()`/`SettingsWatcher::save` must write the file whole and always formatted, every known setting with its
   value (defaults included), unknown members after the known ones, no comments; a second save changes nothing (tests first).
   Today toText() writes the loaded document with only the keys that were set; see `src/frontend/settings.h`.
2. *Daemon side of the performance log* (`docs/tasks/perf-log.md`): pump rounds, bytes, clients, backpressure, CPU, ping, plus the shared log writer
   and verdict. agent3 (xmnca) proposed the header: `perf::Log` (env `MATTERM_PERF_LOG`/`--perf-log`, `line()`, summary at destruction),
   `perf::Window` (add(ms), avg/p95/max/count per second), `perf::verdict(Numbers)` -> "network | daemon | client | host terminal"; I owe an answer
   (accept, put it in `src/perflog.h`, namespace `perf`). Context: the owner sees heavy lag starting neovim over the LAN. The replay fix above is not about that.

**Open questions for the owner**: the theme colours (RGB per theme) have never been seen on a screen (hue only, about grey's brightness);
the settings TUI is agent3's. JSON readers: A (jsonvalue.h is the one client reader, sessionsnapshot moves to it) / B (everything on fast-json, extended
upstream) / C (leave): my recommendation was A now, B later if one library is wanted; the owner has said writing is not centralised and fast-json gets no
comment mode now. Checkpoint on detach (replay step 3) was left as alternatives A/B/C (recommendation: not needed now; B = checkpoint at detach only,
memory only, if slow links matter).

**Gotchas learned**: never run `git worktree` from the sandbox (it pruned my worktree metadata once; repaired by recreating
`.git/worktrees/agent2/{HEAD,commondir,gitdir}`); `git clang-format` can reflow a whole table when a line in it changes and makes later scripted
edits miss; fast-json's nodes are views into the input text (keep it alive) and it needs C++20.
