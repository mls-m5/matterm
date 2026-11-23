# Daemon versions, `matterm-attach`, and stopping an old daemon

Decided by the owner (do not reopen)
- Version of the daemon control protocol is `format.breaking.additive` plus a build hash:
  - **format** (first number, almost never changes): the first bytes of the handshake. Frozen so
    that any version can always say who it is and understand "stop". Today's hello stays readable.
  - **breaking** (second): different values -> the client refuses to connect, explains, and offers
    to start a new daemon next to the old one (the old one keeps running with its terminals).
  - **additive** (third, changes often): different values -> only a note; the newer side must
    handle the older. Replaces ad-hoc feature probing where practical.
  - **build hash**: git SHA of the build, shown in messages and lists, never decides anything.
  The existing `kControlVersion` (3) and `ControlWelcome.build` are the starting point; keep
  working with today's daemons (treat their number as breaking = 3, additive = 0).
- A **stop** that does not depend on compatible protocols: `--daemon-list` (socket, pid, version,
  hash, number of terminals, per daemon found for the user) and `--daemon-stop` (ask the daemon to
  exit when the protocol is compatible; otherwise the pid from the socket peer, `SO_PEERCRED`,
  and SIGTERM). Always asks first and names how many terminals run. Palette entry "Stop daemon...".
  Over ssh it runs the copied `matterm-attach` on the remote computer.
- **`matterm-attach`**: a separate statically linked binary without SDL that does what
  `matterm --attach-tty` does (list, take over, leave key), plus `--daemon-list` and
  `--daemon-stop`. `--daemon-install` copies it together with `matterm-daemon` so both are the same
  version and the ssh way (`ssh -t host matterm-attach`) always works. `matterm --attach-tty`
  stays (same code).
- No secrets or deployment specifics in the repository (it is public).

## agent2
1. Version scheme and the refusal/offer logic (client side in `src/frontend/daemonlink.h`,
   `sshtunnel.h`, daemon side in `src/daemon/control.*`), tests with a faked version both ways.
2. `--daemon-list`, `--daemon-stop` (+ palette entry text goes to agent3's registry, tell them).
3. `matterm-attach` target (static, no SDL; may use agent3's `--attach-tty` code, see below) and
   `--daemon-install` copying it; docs/REMOTE_DAEMON.md and docs/DEPENDENCIES.md updated.
## agent3
1. Swap your own painter/decoder for the shared `src/tty` layer (it is on main now: `matterm::tty`,
   `ttyposix`, `ttymatterm`); delete the duplicates; tests stay green.
2. Bug to reproduce (needs `--daemon-host` with `test/fakessh.py`): "Detach tab", close or restart
   the window, then "Attach layout..." is missing from the palette (`state.layouts > 0`,
   `ptyfrontend.cpp:1148`). Find out whether the layout file was written/read (layouts are saved
   on the window's computer, not the daemon's); fix with a test. If the cause is that layouts
   live on the wrong computer for `--daemon-host`, write the question to the integrator with
   alternatives instead of changing it.
3. Rename "Attach layout..." to "Attach tab..." (match "Detach tab"): palette text, messages,
   tests, docs/TABS_PLAN.md; keep the internal ids.
4. Make the `--attach-tty` code buildable as a separate target without SDL so agent2 can link it
   into `matterm-attach`; tell agent2 when it is.

Both: tests first, tidy clean (`tools/clang-tidy.sh build --changed <base>`; agent3 has no
clang-tidy, the integrator runs it), small commits, feature descriptions in
`docs/handover/features/agentN-<name>.md`, SHAs to the integrator.

## Addendum (owner, 2026-11-23): names and cleanup (agent2)
- The installed file names carry the version: `matterm-daemon-<format>.<breaking>.<additive>-<hash8>` and the same for `matterm-attach-...` (today the name is the content hash). Lists (`--daemon-list`, install messages) sort by the numbers, as numbers (1.10.0 after 1.9.0). A running file is never overwritten.
- Cleanup of old versions is done by the installer (`--daemon-install`) after it has copied the new files: only files matching those two name patterns in matterm's own install directory; never a file that a process runs (`/proc/*/exe`); keep the newest older version as a fallback; no question, but print what was removed. Not done: the daemon cleaning up after itself (maybe later; ask the owner first).
- A new daemon/attach binary must be able to stop older daemons (see the stop part above).
- Several daemons at once (owner's question 2026-11-23): a daemon is identified by its socket. The
  default socket name carries the version (`daemon-<format>.<breaking>.<additive>.sock` next to
  today's `daemon.sock`, which stays working for older builds). `--daemon-list` finds all sockets
  in the runtime directory and asks each (frozen handshake): version, hash, pid, number of
  terminals. Window and `--attach-tty` pick: one socket -> it; several -> the newest compatible one,
  with a question when more than one fits; `--daemon-socket` always wins. "Open next to the old one"
  = a new daemon on its own socket; "stop the old one" names it by socket.
- Stray daemons (owner, 2026-11-23): never kill terminals automatically. When the window or
  `--attach-tty` starts and more than one daemon runs, or one of an older version, show a one-line
  notice ("2 other daemons run (1 old): --daemon-list"). `--daemon-list` also shows how long since
  each daemon had a client. "Stop daemon..." (palette) and `--daemon-stop` ask first and name how
  many terminals run (and the programs when known). Empty daemons already leave by themselves after
  `--idle-seconds` (60 s) with no sessions and no client.
