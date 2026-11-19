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

- [x] Incremental feed(bytes), preserving partial UTF-8 and escape sequences.
- [x] Explicit ground, escape, CSI, string, and discard states; no regex/line parsing.
- [x] Bounded parameters and control-string storage, with safe recovery.
- [x] CR/LF/BS/tab and ordinary Unicode text through the existing model.
- [x] CSI cursor positioning/movement and line/display erasure.
- [x] SGR reset, 16-color, indexed 256-color, and RGB foreground/background.
- [x] Cursor visibility and saved cursor handling.
- [x] Narrow, documented alternate-screen mode support, without claiming full DEC compatibility.
- [x] Bounded OSC title events; ignore clipboard and unsupported control strings safely.
- [x] Explicit reply channel for supported status queries, without a PTY dependency.
- [x] Tests comparing whole input with every split point and byte-at-a-time input.
- [x] Tests for malformed, unknown, oversized, cancelled, and truncated sequences.
- [x] Start a separate matterm application for VT/file input; keep both existing demos unchanged.
- [x] Document the exact supported subset and unsupported behavior in docs/VT.md.

## 3. Broader Terminal Semantics

- [ ] Scrolling regions, insert/delete/erase characters and lines, reverse index.
- [ ] Full text attributes, configurable tab stops, autowrap/origin modes.
- [ ] Distinct DEC private mode semantics (47/1047/1048/1049), beyond the initial subset.
- [ ] Application keypad/mouse modes (application cursor and bracketed paste state/input encoding are implemented).
- [ ] Golden transcripts from real programs, fuzz/property tests, and compatibility fixtures.
- [ ] Wide cells, combining clusters, shaping, and font fallback.

## 4. POSIX PTY Adapter (Implemented on Linux)

- [x] src/platform/posix/: isolated PtyProcess with start/read/write/resize/pollExit.
- [x] forkpty session, controlling-terminal, stdio setup, and pre-exec error reporting.
- [x] Nonblocking master I/O, partial writes, EAGAIN/EINTR, EOF/Linux EIO handling.
- [x] Bounded session read/write work and queued outgoing bytes.
- [x] Initial winsize and TIOCSWINSZ on cell-size changes; tty SIGWINCH delivery tested.
- [x] Child exit/reaping, descriptor cleanup, and explicit shutdown policy.
- [x] Preserve termios/foreground process-group semantics; Ctrl+C delivery tested.
- [x] Controlled-child tests plus an interactive bash command/exit test.

## 5. Terminal Application

- [x] Separate matterm executable using the same model/parser libraries (VT frontend only so far).
- [x] PtySession joins PTY output -> parser -> model -> renderer.
- [x] Keyboard/text/paste -> input encoder -> PTY; parser replies share the queued output path.
- [ ] Capability-appropriate TERM/terminfo; do not advertise xterm-256color prematurely.
- [x] Controlled tty/job-control/resize/exit tests, interactive bash command test, and end-to-end PTY rendering smoke.
- [ ] Broader full-screen/editor transcripts and interactive compatibility testing.
- [x] Keep dangerous OSC actions disabled; any future opt-in requires a security policy.

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

## Future Direction: Emscripten and Browser Clients (Do Not Implement Yet)

All application entry points should eventually support a callback-driven update
loop rather than relying on a blocking native while loop and stack-owned session.

- [ ] Separate initialization, input/update, rendering, and shutdown from main().
- [ ] Give session state an explicit lifetime across browser frame callbacks.
- [ ] Schedule redraws when cells, cursor, viewport, size, or atlas resources change.
- [ ] Request animation frames while scrolling; use timers for cursor blink.
- [ ] Avoid continuously redrawing idle frames; a sleeping native VSync loop is
  not the browser architecture. Pause/resume and cleanup must be explicit.
- [ ] Keep native PTY/ioctl code isolated from portable model/parser/renderer code.
- [ ] Validate SDL/font assets and chosen rendering APIs under Emscripten; do not
  claim the current programs already compile or run in a browser.

Two browser use cases should reuse the same libraries:

- [ ] Remote terminal: a server owns a PTY/process, while a browser owns the parser,
  model, and renderer. Define binary terminal-data, resize, input, status/exit,
  and reconnection messages behind a transport interface (for example WebSocket).
- [ ] Plan authentication, authorization, TLS, origin checks, process isolation,
  backpressure, message bounds, and session cleanup before exposing remote shells.
- [ ] Local simulation: generate scripted/timed VT output entirely in the browser,
  without a server or shell, for company presentations, advertisements, and effects.
- [ ] Preserve direct-cell and terminal-model demos as examples for those clients.

Native PTY, remote transport, and simulated streams are alternative adapters;
none should become a dependency of direct cell rendering. These are recorded
future goals, not implementation in the current parser slice.

## References

- Xterm control sequence reference: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- Linux tty ioctls: https://man7.org/linux/man-pages/man4/tty_ioctl.4.html

These describe much more functionality than matterm currently implements.
