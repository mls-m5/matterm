# Linux PTY and Session Layer

`matterm::pty` provides `PtyProcess`; `matterm::session` adds a `TerminalModel`,
`VtParser`, bounded outbound queue, and a nonblocking pump. Neither requires SDL.
Build with MATTERM_ENABLE_PTY on Linux; disable it for portable/browser clients.
This is a native adapter, not a remote-shell server or a process sandbox.

## Process Ownership

- forkpty establishes the child session, controlling terminal, and stdio.
- Arguments, executable PATH resolution, and environment storage are prepared
  before fork. The child uses execve and reports exec errors through a CLOEXEC pipe.
- TERM defaults to xterm-256color. Inherited COLORTERM/COLUMNS/LINES
  are removed so they cannot falsely override the chosen capabilities or size.
  The native application explicitly advertises `COLORTERM=truecolor`; generic
  PTY library clients can opt in. TERM is not changed by that choice.
- The master is nonblocking and close-on-exec. Callers queue partial writes.
- Initial winsize is supplied to forkpty. TIOCSWINSZ handles resize, including the
  tty driver's SIGWINCH delivery to the foreground process group.
- The tty driver handles canonical input, echo, ISIG, and foreground jobs. Sending
  ETX (Ctrl+C) is not the same as directly sending SIGINT to an arbitrary child.
- Reads distinguish data, EAGAIN, and EOF/Linux EIO. Child exit does not mean all
  buffered output has been drained. Failed writes do not discard the master.
- pollExit caches/reaps the direct child. Do not waitpid it elsewhere.
- Shutdown closes the master, sends HUP to the child/group, allows 100 ms grace,
  then kills/reaps a non-exiting child. It is not containment of arbitrary daemonized
  descendants; a future remote service requires stronger isolation and policy.

Prefer starting the process before graphics or worker threads. Other inherited
application descriptors should be close-on-exec. No shell string is implicitly
evaluated: the adapter accepts an argument vector. Invoke a shell explicitly when
shell syntax is intended.

## Session Pump and Input

PtySession reads and writes at most 64 KiB each per pump by default. Parser replies
join the same outbound queue as keyboard data. At most 1 MiB may remain queued;
send() returns false when closed/full, and reply overflow is an explicit error.
EOF flushes the parser once; finished() requires both EOF and a reaped child.
The visible model remains available after exit. Animation/blink/render scheduling
belongs to the application, not the transport.

InputEncoder provides GUI-independent special keys, Ctrl/Meta combinations,
application-cursor sequences, and bracketed paste. The parser tracks private modes
1 and 2004. Paste strips ASCII C0/DEL controls other than tab/newlines (including
ESC), normalizes newlines to CR, and uses bracket markers only when negotiated.
Application keypad and mouse reporting are not implemented yet.

## Tests

Controlled children verify controlling tty ownership on all stdio descriptors,
initial size, termios input, Ctrl+C delivery, SIGWINCH/resize, nonblocking reads,
exec failure, shutdown/reaping, parser replies, and complete output draining.
A separate interactive bash test uses --noprofile --norc, sends a command, and
checks its output and exit status. When available, optional tests run a real
Python curses screen (including DEC borders and keyboard input) and top. These
are integration smoke tests, not comprehensive editor compatibility claims.
Rendering/GPU behavior is tested separately.

## Native Application

The separate matterm application launches a shell on Linux by default. `-- PROGRAM
ARG...` selects a command; `--term NAME` overrides the xterm-256color default.
`--showcase` and `--input FILE` select transport-free VT viewing instead. The child
starts before SDL initialization; the real cell size then triggers model/PTY resize.

Special keys/Ctrl/Meta and UTF-8 text are encoded to the PTY. AltGr is treated as
text input, not an extra Escape prefix. Shift+Page Up/Down navigates history;
ordinary page/function/Escape keys go to the child. Ctrl+Shift+V explicitly pastes.
Ctrl+Shift+Q or closing the window shuts down the session. The window also
closes automatically once the child is reaped and PTY output is drained.

## Recording native PTY output

```sh
./build/matterm --record-output build/pi-color.vt
./build/matterm --input build/pi-color.vt
```

In the first window, run the application and reproduce the problem; exit the
shell to finish. The optional flag also works with `-- COMMAND ARG...`. It saves
raw bytes read from the PTY before parsing, without wrapping the child in another
PTY. It does not separately record keys or replies sent to the child, but echoed
input and any sensitive output can appear in the file. Use a fresh filename: the
file is created with owner-only permissions and will not replace an existing
file. Capture stops at 64 MiB or a write error; the session continues with a
warning on stderr. Recording is synchronous; use a local file and leave it off
for benchmarks. The stream contains no timestamps or resize events; `--input`
plays it with the current window geometry and does not run the original program.
Do not commit or share a recording without checking for sensitive content.
Mouse wheel navigates primary history or sends arrow keys on the alternate screen;
full mouse reporting/selection is not implemented.

A separate GUI smoke test verifies the real child -> PTY -> parser -> model -> SDL
path, including a status-query reply back to the child. Both portable demos remain
unchanged. See ROADMAP.md for remaining compatibility and future web transports.
