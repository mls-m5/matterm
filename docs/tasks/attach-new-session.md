# `matterm-attach` as a small tmux: start a terminal, leave it running, come back

Owner's wish (2026-11-23): `matterm-attach` (and `matterm --attach-tty`) should be usable without
any matterm window: start a new terminal (bash) in the daemon, work in it, detach from it, and
attach again later. Together with what exists (list, take over, leave key Ctrl+], versioned
daemons) this is a minimal tmux.

Decided by the owner
- Also fix the feedback loop when attaching to the terminal you are running in (see below).
- The build hash gets a `-dirty` suffix when the working tree has uncommitted changes
  (`abcd1234-dirty`; `unknown` without git as before). It is still taken when cmake runs.

Proposed defaults (the owner can change them; if you see a better one, ask with alternatives)
- The list gets a first entry "New terminal" (program: the user's shell; folder: current
  directory). `--new [--name NAME] [-- COMMAND...]` starts one directly and attaches to it,
  without the list. The daemon is started (or a compatible one reused, as `--ensure` does) when
  none runs.
- Detach = what exists: Ctrl+] to the list, `q` to leave; the terminal keeps running in the
  daemon. Also a direct one-key way out is welcome if it is cheap (propose it: e.g. Ctrl+] then
  `d`), documented in the one-line hint. Leaving never ends the program.
- When the program in a terminal ends, the client says so ("the program ended, exit code N") and
  returns to the list (or exits when started with `--new`).

## Feedback loop (agent2 + agent3)
Attaching to the terminal you are running in makes the output feed itself and Enter never gets
you out (observed). Fix: the daemon sets `MATTERM_SESSION=<id>` in the environment of every
terminal it starts (agent2). The attach modes mark that session in the list as "(this terminal)"
and refuse to pick it with a short explanation; the window's "Attach terminal..." does the same
(agent3). Test: start a session, run the attach mode inside it, expect the refusal.

## Split
- agent2: `MATTERM_SESSION`, `-dirty` in CMake, daemon side of creating a terminal for a client
  that has no window (CreateSession exists since control v2; size, cwd, command, name), starting
  or reusing the daemon from the attach client.
- agent3: list entry "New terminal", `--new/--name`, ended-program state, refusal of the own
  terminal, the leave keys and hint, option tables and completion, README and docs/REMOTE_DAEMON.md.

Rules as before: tests first, small commits, tidy clean, a feature description in
`docs/handover/features/agentN-<name>.md`, SHAs to the integrator, design questions with
alternatives to the integrator. State what was not run (no phone, no real sshd).

## Addendum (owner, 2026-11-23): name, keys, status line
- The tool is called **matmux** (stable name; versioned files `matmux-F.B.A-hash8`). `matterm-attach` is renamed to it after the running tasks are integrated (integrator coordinates; do not rename yet). `matmux` without arguments starts a new terminal with the user's shell and attaches; `matmux --attach` lists and chooses; `matmux --new [--name N] [-- COMMAND...]`; `--daemon-list`/`--daemon-stop` as before.
- The prefix key is **Ctrl+B**, the same as the window's (`src/frontend/prefixkeys.h`, `forwardPrefix`). After it: `d` leave (the terminal keeps running, the client ends), `l` back to the list, `c` new terminal, `s` status line on/off, `?` help; **Ctrl+B twice sends one Ctrl+B to the program** (owner approved). Ctrl+] stays as a spare until the owner says otherwise.
- After the prefix a short hint line shows the possible keys at the bottom (temporary overlay, the program's size does not change; gone at the next key).
- Optional permanent status line (name of the terminal, prefix and the main keys). It takes one row, so the pty size sent to the daemon is rows-1 while it shows. Default on from 24 rows, off below (owner to confirm); toggled with Ctrl+B s.
