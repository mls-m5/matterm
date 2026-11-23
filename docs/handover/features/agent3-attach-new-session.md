# A small tmux: new terminal, prefix keys, status line (`--attach-tty`, later `matmux`)

Written by agent3, 2026-11-23. Contract: docs/tasks/attach-new-session.md (section agent3).

## What it does

`matterm --attach-tty` and `matmux` (same code, library `matterm::attachtty`):

- The list has **New terminal** as the first row (`n`): your shell, in the current folder (not for a
  daemon on another computer: its own folder). With terminals present the chosen row starts on the
  first terminal, so Enter still opens that.
- `--new [--name N] [-- COMMAND...]` starts a terminal and attaches at once, no list; the daemon is
  started or a compatible one used (`selectDaemon(..., startMissing)`). The program ending ends the
  client with its exit code and one line ("The program in N ended (exit code 3)"). `-- COMMAND` runs
  the command instead of the shell (agent2's `CreateTerminal` via `frontend::createTerminal`, which
  falls back to `CreateSession` on an older daemon when there is no command or name).
- In the list, an ended program brings the list back with the same sentence.
- **Own terminal**: the daemon will put `MATTERM_SESSION=<id>` in each terminal's environment
  (agent2). The list marks that terminal `(this terminal)` and refuses it ("You are in this terminal:
  attaching it would feed it to itself"); the window's Attach terminal... / Attach terminal here...
  do the same (`AttachableSession::thisTerminal`, `thisTerminalNotice()`).
- **Keys** (owner's addendum): prefix **Ctrl+B**; then `d` leave (client ends, terminal runs on, one
  line says so), `l` list, `c` new terminal, `s` status line, `?` help overlay, Ctrl+B twice sends one
  Ctrl+B. Ctrl+] stays as a spare for the list. After Ctrl+B a one-line overlay names the keys until
  the next key (that key is not sent). Works in the taken-over/lost states as well.
- **Status line**: name of the terminal and the main keys, bottom row; on by default from 24 rows,
  off below; the program gets rows-1 (the size sent to the daemon follows, also when toggled).
  The old first-time "Ctrl+] leaves" hint remains for when the status line is off.

## Found on the way

A client started with `--new` hung until the first key: the connection says hello only when it is
first served (the loop waits for input or readable data otherwise). The pane is now served at once
after attach and after Enter-to-reconnect (`AttachTty::open`); test "A new connection is served at
once".

## How to try

`matterm --attach-tty --new` in a terminal: type, Ctrl+B ?, Ctrl+B s, Ctrl+B d; then
`matterm --attach-tty` and choose it. Run `matterm --attach-tty` inside that terminal: the list shows
`(this terminal)`.

## Verified / not verified

Verified: 36 mode tests with fakes, list/input/cli tests, `test/attachtty_pty.py` (real daemon, real
ptys, both `matterm` and the SDL-free probe): new terminal from the list and with `--new`, name, Ctrl+B
d, exit code, own terminal refused (env set by the test; a real terminal gets it from the daemon), status line size, takeover both ways.
The pty test also checks `-- COMMAND` with its exit code, `MATTERM_SESSION` set by the daemon, and the mouse on/off in the host terminal. Not verified: a real phone,
the window's own-terminal refusal on a screen (only the list text is tested), `matmux` names (not
renamed yet), the config-file setting for the status line (no config file yet).
Diff-based drawing means the pty test looks for short fragments of the lines on screen.
