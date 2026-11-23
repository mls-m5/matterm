# `--attach-tty`: take over a running session in the terminal you are in

Owner's goal: reach sessions that already run in `matterm-daemon` from a phone (Termux, ssh to the
computer) without a browser, a server or an open port. `matterm --attach-tty` runs on the computer
that has the daemon (or through `--daemon-host`), lists the running sessions, takes one over and
shows it in the terminal it was started from. The phone only needs an ssh client.

Decided by the owner (do not reopen)
- Option name: **`--attach-tty`** (two dashes, like the other options).
- One pane at a time, full screen. No splits, tab strip or window management in this mode
  (layouts stay in the window). A simple list to choose which session/pane.
- Takes over like the window does: the other client is kicked temporarily ("taken over, press
  Enter to take it back", PROTOCOL.md 11.3, TABS_PLAN.md section 13-14). No extra confirmation:
  choosing the session in the list is the confirmation.
- The client emulates the terminal itself (matterm's own cell buffer) and draws the cell buffer
  to the host terminal. It does not forward raw bytes: full-screen programs (alternate screen)
  must look right, also after a reconnect (replay of the daemon's buffer rebuilds the screen).
- After a dropped connection: running the command again takes the session over again and the
  screen is rebuilt from the replay. No checkpoints (docs/MULTI_TERMINAL.md section 12 stays an idea).
- The terminal drawing layer is **shared**: the demo's `--tty`, `mattrix` and `mattrix-chat` use
  the same code as `--attach-tty` (no second copy). The owner wants it to become a small library
  later (a better ncurses: cell buffer in, any terminal out, input decoded), so keep its
  interface clean and free of matterm-specific types where possible. No new dependencies.
- Pushing is the owner's job; the repository is public (no deployment instructions or secrets).

## Part 1 (agent2, first): the shared tty layer
Move what the demo's tty mode has (cell diff to ANSI with the smallest cursor and colour
updates, colour reduction to truecolor/256/16, capability detection, raw mode that restores,
window size, robust write, key and mouse decoding) from `src/fakeos/canvas.*`,
`app/demo/ttyterm.*` and `src/fakeos/keys.*` into `src/tty/` (new directory, own CMake target).
The demo uses it afterwards; its tests (`fakeos_*`, `demotty_test`) stay green and unchanged in
behaviour. Publish the header (interface) to agent3 by message as soon as it compiles, before the
rest is finished.

## Part 2 (agent2): draw matterm's terminal
A renderer in `src/tty/` that takes matterm's cell buffer (`src/core`: attributes, colours, wide
and zero-width characters, cursor, alternate screen) and draws it with the diff output; handles
host terminal resize and `SIGWINCH`, restores the host terminal on exit/crash/stop signals.
Tests: pure (cell buffer in, expected bytes out) and one under a real pty like `demotty_test`.

## Part 3 (agent3): the mode itself
- Option `--attach-tty` in `src/frontend/clioptions.h` (and the completion generators), combined
  with `--daemon-socket` and `--daemon-host` (reconnect and tunnel code already exist).
- A session/pane list (reuse the picker UI model if it fits, otherwise a plain list drawn with
  the part-1 layer): name, what runs, whether another client has it. Choose with keys; `q`/Esc
  quits. If there is only one, still show the list (no surprises).
- Takeover, the "taken over" state when kicked by another client, Enter to take back, input
  forwarding (keys, mouse where the terminal sends it, bracketed paste), resize forwarding.
- A short key (documented) to leave the pane and go back to the list; pick one that programs
  rarely use and show it in a one-line hint the first time.
- Docs: README, docs/REMOTE_DAEMON.md (phone via ssh, exact commands, no secrets), completion.
- Until part 1 is published, develop against a small interface of your own and say so.

Owned paths: agent2 `src/tty/`, `src/fakeos/canvas.*`, `src/fakeos/keys.*`, `app/demo/ttyterm.*`,
tests for them. agent3 `src/frontend/` additions for this mode, `app/` entry for `--attach-tty`,
clioptions, docs. Shared: `CMakeLists.txt` (small edits, report them).

Acceptance (each part): ctest green, tests fail before and pass after, tree warning-free,
clang-format and `tools/clang-tidy.sh build --changed <base>` clean, a feature description in
`docs/handover/features/agentN-<name>.md` written for the owner (what it does, how to try it,
what was not verified), small commits, SHA reported to the integrator. Design questions: write them
to the integrator with 2-3 alternatives and a recommendation, the owner decides.
State plainly what was not run (no real phone, no real sshd; real-pty tests are fine).
