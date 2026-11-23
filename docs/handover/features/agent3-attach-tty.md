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

## The shared tty layer and the SDL-free library (2026-11-23)

The mode draws through agent2's `src/tty` now: `HostScreen` hands out a `tty::Screen`; the list is
drawn into its canvas, the pane with `tty::drawCells` (indexed/default colours go to the host
terminal's theme, wide characters are handled by the canvas); `app/attachtty.cpp` runs a
`tty::Session` (raw mode, alternate screen, SIGWINCH, SIGTERM/SIGHUP, stop/continue, restore after a
crash). My painter (`ttyplain.h`) and my picture type are gone.

`ttyinput.h` stays: `tty::KeyDecoder` has no function keys, Insert or modifier bits on arrows, and a
program in the pane needs those; the file also re-encodes everything for the program's modes
(`InputEncoder`). If `tty::Key` grows those, only the decoding half can move.
The mouse is asked for in `TtyHost` itself, only while the program wants it, because
`tty::ScreenOptions::mouse` is all or nothing; those escapes are written directly and undone by
`TtyHost::restore()` (a crash handler does not know them: a crash while the program had the mouse on
leaves it on in the host terminal; a `setOptions` on `tty::Screen` would close that gap).

The mode, its daemon client and the tty layer are the CMake library `matterm::attachtty`
(`app/attachtty.cpp`, no SDL; entry `runAttachTty(DaemonOptions)` in `app/attachtty.h`). `matterm`
links it; `test/attachtty_main.cpp` (`attachtty_probe`) is a program of only that, and the pty test
runs against both (`attach_tty_pty`, `attach_tty_pty_nosdl`; `ldd` of the probe shows no SDL).

## Verified

- Unit tests: ttyinput (10), ttylist (12), ttyattach (20 with fakes), cli options.
- `test/attachtty_pty.py` (ctest `attach_tty_pty`, needs the build with `-DMATTERM_BUILD_SERVER=ON`):
  real `matterm-daemon` (the e2e helper) and real ptys: list, open, replay, typing, size, resize,
  two clients taking over from each other, Enter to take back, leave key, `q` with the terminal
  restored, Ctrl+C, no daemon.
- Whole ctest in the server build (116 tests) passed.

## Not verified

- A real phone, Termux, a real sshd (only ptys here). Mouse reports and bracketed paste were tested as
  bytes into the decoder and into the mode with fakes, not by a real terminal.
- Wide characters, emoji and combining marks go through the layer's canvas but were not looked at on a real terminal; other TERM values than xterm-like.
- clang-tidy (not installed here) and zsh/fish completion (only the generated text is tested).
- A tunnel (`--attach-tty --daemon-host`): same code as the window's, not run here.
