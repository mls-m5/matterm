# matmux passthrough: the host terminal's own scrollback

Written by agent3, 2026-11-23. Contract: docs/tasks/matmux-passthrough.md.

## What it does

`matmux` (all ways: no arguments, `--new`, `--attach`) shows a terminal by passing the daemon's bytes
through to stdout unchanged. The host terminal (Termux, GNOME Terminal) keeps its scrollback, selection,
mouse and touch scrolling. `matmux --rendered` and `matterm --attach-tty` keep the rendered mode
(status line, cell diff, alternate screen); nothing of it was removed.

- **Attach**: the daemon's replay goes into a hidden model sized to the host; when it has all arrived
  (the list's `outputEnd` of the terminal is the mark, 5 s at most) one snapshot is written (agent2's
  `tty::snapshot`: scrollback as lines, `rows` screen lines, cursor, modes, or the alternate screen), then
  every byte goes through. Input typed meanwhile waits for it. One line with the keys is printed first
  (not on later attaches in the same run).
- **Live**: raw forwarding; keys, mouse and paste are sent to the program as the host sends them (the host
  was told the modes by the program's own sequences). Backpressure: when 1 MiB waits for the host the
  daemon is not read (nothing is dropped; the daemon holds back); input while backed up waits for the
  host as well. A resize goes to the daemon.
- **Prefix** Ctrl+B: `d` leave (exit 0, "keeps running" line), `l` list, `c` new terminal, Ctrl+B twice sends
  one; Ctrl+] is the spare for the list. Other keys after Ctrl+B only cancel.
- **Ends**: Leave, list, new, the program ending (plain line; with `--new`/no arguments the client exits
  with its exit code; from the list it goes back to the list with the line), taken over / connection lost
  (line, then Enter takes it back = a new attach with a new snapshot, `l`, `d`), gone. The host terminal is
  restored (agent2's `tty::hostRestore`: alternate screen left if the program had it, reports off, cursor
  shown, SGR reset, newline) on every one of these, on SIGTERM/SIGHUP/SIGINT, on EOF of the terminal, and in
  a crash handler (SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL) with the sequence kept up to date.
- The **list** is the rendered list in the alternate screen, left before a terminal is shown.

## Proposal on the hint line (the task asked for a proposal)

No hint when the prefix is pressed and no `Ctrl+B ?` help while a terminal is shown: any line printed
there would be inside the program's screen (scrolling it, or overwriting its last row if it is saved and
restored with ESC 7/8, which a program in the alternate screen would not survive). Instead one plain line
before the snapshot on the first attach (and `matmux --help`). Alternatives, if the owner wants more:
(a) the window title (OSC 2) while the prefix is pending: invisible in Termux; (b) overwrite the bottom row
and restore it from the model afterwards: works for the normal screen, flickers; (c) a bell. Recommendation: keep it as is.

## Not yet the real snapshot

`app/passthroughrun.cpp` uses agent2's `tty::snapshot`/`hostRestore` when `src/tty/matterm/snapshot.h` is
there (`__has_include`), else a stand-in (the visible screen as plain lines, the cursor, the alternate
screen): no scrollback, no attributes, no modes. That is what is tested here so far. The core
(`src/frontend/passthrough.h`) takes the two functions as `std::function`s.

## Measured (pty test, stand-in snapshot)

A new terminal that prints 20000 lines and then a marker: the last line is on the host terminal after
0.4 s (`test/matmux_pty.py` prints it). A long history on re-attach and vim/htop in both screens: not
measured with the real snapshot yet.

## Verified / not verified

`test/passthrough_test.cpp` (12: replay then snapshot then unchanged bytes, nothing twice, replay timeout,
input raw and waiting, prefix commands and cancelling and a split prefix, ended/taken over/lost/gone,
backpressure, resize, restore with the alternate-screen flag); `test/matmux_pty.py` (ctest `matmux_pty`,
real daemon, real ptys): long history, no alternate screen unless the program uses it, the list uses it and
leaves it, Ctrl+B d/l, typing and `stty size`, resize, `exit 4` -> exit code 4, takeover line and restore,
SIGTERM restore. Not verified: a phone, Termux's own scrolling (only the bytes are checked), the real
snapshot and its limits (tab stops, charsets, pen, pending wrap, the primary screen behind an alternate one),
a crash restore (the handler is written and not exercised), typing ahead during the attach on a slow link.
