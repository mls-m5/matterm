# `matterm --attach-tty`

Written by agent3, 2026-11-23. Part 3 of docs/tasks/attach-tty.md (the mode itself).

## What it does

`matterm --attach-tty` runs without a window in the terminal it was started from. It lists the
terminals that run in `matterm-daemon` (name, what runs, `(in use)` when another client has it), you
choose one, and it is shown full screen; the keys you type go to it. Ctrl+] goes back to the list,
q/Esc/Ctrl+C in the list quit. Taken over by another client: the screen stays, a line at the bottom
says "Taken over by another client. Enter: take it back"; Enter takes it back (and the other one is
kicked in turn). A lost connection says so and Enter connects again. After a dropped ssh, running the
command again takes the terminal over and the daemon's replay rebuilds the screen. Works with
`--daemon-socket` and `--daemon-host`. Documented for a phone in docs/REMOTE_DAEMON.md section d.

## How to try

On the computer (daemon running, e.g. a window open): in another terminal
`matterm --attach-tty`, or from the phone `ssh -t you@computer matterm --attach-tty`. Try: choose a
terminal in which `vim` or `htop` runs; type; turn the phone (resize); open the same terminal in the
window (the phone shows the taken-over line, Enter takes it back); Ctrl+] and `q`.

## How it is built

- `src/frontend/ttyattach.h`: the mode (list / pane / taken over), against three small interfaces:
  `HostScreen` (size, show a picture, which reports to ask the host for), `SessionSource` (list, open),
  `AttachedPane` (a TerminalView on a DaemonLink). Tested with fakes (`ttyattach_test`).
- `ttyinput.h`: decodes the host terminal's bytes (keys, SGR mouse, bracketed paste, the leave key) and
  encodes them again with `InputEncoder` for the program's modes (application cursor keys, mouse
  encoding, paste brackets), as the window does. `ttylist.h`: list model and drawing. `ttyscreen.h`:
  the picture (`ScreenImage`). `ttyplain.h`: paints a picture to a terminal (changed rows only).
- `app/attachtty.cpp`: raw mode, alternate screen, SIGWINCH/SIGTERM/SIGHUP, the poll loop, the
  daemon source. `app/daemonconnect.h`: the ssh tunnel for `--daemon-host`, moved out of
  `ptyfrontend.cpp` (behaviour unchanged) so both use it.
- The leave key Ctrl+] (0x1d) is one constant (`kLeaveByte`, ttyinput.h); telnet's escape key,
  rarely used by programs. A design alternative if the owner dislikes it: Ctrl+\ (but ssh clients
  and terminals map it to SIGQUIT in cooked mode; here it would work too since raw mode is on).

## Not the shared tty layer yet

agent2's `src/tty/` (parts 1 and 2) was announced but is not on `main`. Until it is, drawing and key
decoding in this mode are my own small versions (`ttyplain.h`, `ttyinput.h`), behind `HostScreen` and
`ScreenImage`. Swapping: implement `HostScreen` with `tty::Session`/`Screen` (map `ScreenImage` cells
to `tty::Cell`, cursor via `setCursor`) and drop `PlainHost`/`ttyplain.h`; `ttyinput.h` stays useful
for the re-encoding to the program's modes unless `tty::KeyDecoder` replaces its decoding half.

## Verified

- Unit tests: ttyinput (10), ttylist (12), ttyattach (20 with fakes), ttyplain (10), cli options.
- `test/attachtty_pty.py` (ctest `attach_tty_pty`, needs the build with `-DMATTERM_BUILD_SERVER=ON`):
  real `matterm-daemon` (the e2e helper) and real ptys: list, open, replay, typing, size, resize,
  two clients taking over from each other, Enter to take back, leave key, `q` with the terminal
  restored, Ctrl+C, no daemon.
- Whole ctest in the server build (110 tests before this part) passed.

## Not verified

- A real phone, Termux, a real sshd (only ptys here). Mouse reports and bracketed paste were tested as
  bytes into the decoder and into the mode with fakes, not by a real terminal.
- Wide characters, emoji, combining marks (shown as one cell each), other TERM values than xterm-like.
- clang-tidy (not installed here) and zsh/fish completion (only the generated text is tested).
- A tunnel (`--attach-tty --daemon-host`): same code as the window's, not run here.
