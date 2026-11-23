# matmux: pass the program's output through (the host terminal's own scrolling)

Owner's wish (2026-11-23): `matmux` always runs in the alternate screen and cannot be scrolled.
It should forward what the program in the terminal writes, so that the host terminal (Termux,
GNOME Terminal) keeps its own scrollback, selection, mouse and touch scrolling. A status line at
the bottom is not possible then (the program owns the whole screen): accepted.

Decided by the owner (do not reopen)
- **Passthrough is the default way to show a terminal.** If the program is not in the alternate
  screen, matmux is not either; if the program uses it, the program's own sequences switch the host
  terminal (they are passed through). Repeated lines in the host scrollback are fine, as long as
  programs that edit lines a little way back (cursor up, e.g. Claude Code) keep working.
- The list (`matmux --attach`) keeps its own full-screen view; it leaves the alternate screen
  before the terminal is shown, so the host scrollback is kept.
- The rendered mode (what exists now: cell diff, status line, own frames) stays behind
  `--rendered`; nothing of it is deleted. `matterm --attach-tty` keeps the rendered mode.
- Prefix Ctrl+B stays (d leave, l list, c new, Ctrl+B twice sends one): the client still reads
  the input and forwards it. No status line in this mode; what the status line showed (hints) is
  printed once as a plain line on attach and when the prefix is pressed, if it can be done without
  disturbing the program's screen (propose; if not, drop it and say so).

## The attach step (the hard part): a snapshot, not a replay of the whole ring
1. Read the daemon's replay into a hidden model (the same TerminalModel as now; ~0.05 s per 3 MiB).
2. Write to the host: (a) the model's scrollback as ordinary lines (they land in the host's
   scrollback), (b) then the current screen as `rows` plain lines so that the visible area equals
   the screen with the same row alignment (programs that edit lines a little way back use rows on
   the visible screen), (c) set cursor position, attributes, scroll region, charset where cheap,
   and the host input modes the program has asked for (application cursor/keypad, bracketed paste,
   mouse tracking and encoding, focus reporting, cursor visibility); (d) if the program is in the
   alternate screen: enter it on the host and draw the screen there instead of (a)-(b).
3. From then on: the daemon's bytes go to stdout unchanged (in order, nothing dropped), input goes
   the other way. A resize is forwarded to the daemon; the program redraws itself.
4. On leave, on takeover (kicked), on the program ending and on SIGTERM/SIGHUP/crash: restore the
   host terminal (leave alternate screen if the program entered it, mouse/paste/focus off, cursor
   visible, SGR reset, a newline) and print the plain lines the rendered mode printed.
Known limits to write down, not to hide: what the model does not know (tab stops, some charset
state) can differ after attach.

## Split
- agent2: the snapshot writer in `src/tty` (model -> bytes for steps 2 (a)-(d)), with tests: feed a
  program's output through a model, write the snapshot into a second model (and a pty test), the
  screens must be equal; alternate-screen and normal-screen cases; modes; wide characters; and the
  restore sequence of step 4. Publish the header to agent3 early.
- agent3: the mode in `matmux` (and `matterm --attach-tty --passthrough` is NOT needed): live loop,
  raw forwarding with backpressure, input and prefix, takeover/ended/kicked lines, `--rendered`,
  options and completion, README and docs/REMOTE_DAEMON.md (phone). Until agent2's snapshot exists,
  develop against a small interface and say so.
- Measure: reattach to a terminal with a long history and with a full-screen program (vim/htop)
  in both screens, in a real pty test; time to first screen.

Rules as before: tests first, small commits, tidy clean, feature descriptions in
`docs/handover/features/agentN-<name>.md` for the owner, SHAs to the integrator, design questions
with 2-3 alternatives and a recommendation. State what was not run (no phone, no real sshd; tested
in a pty and in Termux-like settings only through the pty tests).
