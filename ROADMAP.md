# matterm Implementation Checklist

This is the working plan. Check items only after implementation and tests.
The renderer, terminal model, and protocol/OS adapters remain separately usable.

## 1. Boundaries and Existing Foundation

- [x] Direct cell drawing, bounded block-backed history, and borrowed row views.
- [x] SDL atlas rendering, cursor overlay, scrollbar, and fractional scrolling.
- [x] CPU terminal model with cursor, colors, erasure, wrapping, primary/alternate screens.
- [x] Keep matterm-demo and matterm-terminal-demo as separate permanent clients.
- [x] Group common cells/storage/view state under src/core/.
- [x] Group graphical backends under src/render/.
- [x] Keep screen behavior under src/terminal/ and stream parsing under src/terminal/vt/.
- [x] Keep the UTF-8 decoder independent of the model and renderer.

## 2. First VT Stream Parser (Current Implementation Slice)

- [ ] Incremental feed(bytes), preserving partial UTF-8 and escape sequences.
- [ ] Explicit ground, escape, CSI, string, and discard states; no regex/line parsing.
- [ ] Bounded parameters and control-string storage, with safe recovery.
- [ ] CR/LF/BS/tab and ordinary Unicode text through the existing model.
- [ ] CSI cursor positioning/movement and line/display erasure.
- [ ] SGR reset, 16-color, indexed 256-color, and RGB foreground/background.
- [ ] Cursor visibility and saved cursor handling.
- [ ] Narrow, documented alternate-screen mode support, without claiming full DEC compatibility.
- [ ] Bounded OSC title events; ignore clipboard and unsupported control strings safely.
- [ ] Explicit reply channel for supported status queries, without a PTY dependency.
- [ ] Tests comparing whole input with every split point and byte-at-a-time input.
- [ ] Tests for malformed, unknown, oversized, cancelled, and truncated sequences.
- [ ] Start a separate matterm application for VT/file input; keep both existing demos unchanged.
- [ ] Document the exact supported subset and unsupported behavior.

## 3. Broader Terminal Semantics

- [ ] Scrolling regions, insert/delete/erase characters and lines, reverse index.
- [ ] Full text attributes, configurable tab stops, autowrap/origin modes.
- [ ] Distinct DEC private mode semantics (47/1047/1048/1049), beyond the initial subset.
- [ ] Application cursor/keypad modes and bracketed paste state/input encoding.
- [ ] Golden transcripts from real programs, fuzz/property tests, and compatibility fixtures.
- [ ] Wide cells, combining clusters, shaping, and font fallback.

## 4. POSIX PTY Adapter (Not Part of the First Parser Slice)

- [ ] src/platform/posix/: isolated PtyProcess with start/read/write/resize/pollExit.
- [ ] forkpty or equivalent session, controlling-terminal, and stdio setup.
- [ ] Nonblocking master I/O, partial writes, EAGAIN/EINTR, EOF/Linux EIO handling.
- [ ] Bounded read/write work per GUI frame and queued outgoing bytes.
- [ ] Initial winsize and TIOCSWINSZ on cell-size changes; rely on tty SIGWINCH behavior.
- [ ] Child exit/reaping, descriptor cleanup, and explicit shutdown policy.
- [ ] Preserve termios/foreground process-group semantics: do not simulate Ctrl+C/echo in the renderer.
- [ ] Tests with controlled child processes before launching a real interactive shell.

## 5. Terminal Application

- [ ] Separate matterm executable using the same model/parser libraries.
- [ ] TerminalSession joins PTY output -> parser -> model -> renderer.
- [ ] Keyboard/text/paste -> input encoder -> PTY; parser replies use the same queued output path.
- [ ] Capability-appropriate TERM/terminfo; do not advertise xterm-256color prematurely.
- [ ] Shell, job-control, resize, alternate-screen, and exit integration tests.
- [ ] Keep dangerous OSC actions disabled unless explicitly opted in with a security policy.

## Future Direction: Output to an Existing Terminal (Do Not Implement Yet)

The direct matrix/drawing API should eventually also support the reverse direction:
render a cell frame as ANSI/VT commands into an existing terminal, as a potential
ncurses alternative. This is a separate output backend/encoder, not the input parser.

- [ ] Design a terminal-output backend accepting the same cell/frame description.
- [ ] Capability-aware colors, cursor control, frame diffing, resize, and alternate-screen lifecycle.
- [ ] Explicit ownership/restoration of the host terminal's modes and input handling.
- [ ] Reuse common cell/style semantics where useful; do not force SDL or a PTY on this client.

Record this direction now, but defer implementation and API commitments until the
input/emulation path is established. Shared-memory/IPC adapters are also future work.

## References

- Xterm control sequence reference: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- Linux tty ioctls: https://man7.org/linux/man-pages/man4/tty_ioctl.4.html

These describe much more functionality than matterm currently implements.
