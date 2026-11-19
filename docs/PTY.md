# Linux PTY and Session Layer

`matterm::pty` provides `PtyProcess`; `matterm::session` adds a `TerminalModel`,
`VtParser`, bounded outbound queue, and a nonblocking pump. Neither requires SDL.
Build with MATTERM_ENABLE_PTY on Linux; disable it for portable/browser clients.
This is a native adapter, not a remote-shell server or a process sandbox.

## Process Ownership

- forkpty establishes the child session, controlling terminal, and stdio.
- Arguments, executable PATH resolution, and environment storage are prepared
  before fork. The child uses execve and reports exec errors through a CLOEXEC pipe.
- TERM defaults conservatively to vt100; COLORTERM/COLUMNS/LINES are removed so
  inherited environment values do not falsely override the chosen capabilities/size.
  This is still a limited implementation, not a VT100 conformance claim.
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
checks its output and exit status. Rendering/GPU behavior is tested separately.

The portable demos remain unchanged. See ROADMAP.md for application integration,
remaining VT compatibility, and future web transports.
