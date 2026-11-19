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

- [x] Scrolling regions, insert/delete/erase characters and lines, reverse index.
- [x] Autowrap/origin/insert modes, inverse/conceal, DEC line graphics, RIS, and conservative primary DA.
- [x] Bold/italic atlas variants, underline/strike/faint/blink, configurable tab stops, and cursor shape/blink controls.
- [ ] Remaining attributes and broader conformance testing.
- [ ] Distinct DEC private mode semantics (47/1047/1048/1049), beyond the initial subset.
- [ ] Application keypad modes (application cursor and bracketed paste are implemented).
- [x] Local mouse drag selection and UTF-8 clipboard copy across scrollback, joining soft wraps.
- [ ] Application mouse input: press/release including right-click, wheel, drag/motion, modifier override for local selection, and SGR reporting.
- [ ] Local right-click Copy/Paste menu and drag auto-scroll (word selection is implemented below).
- [x] Shared native/browser selection: Shift+click/drag preserves the anchor, Ctrl+Shift+A selects retained history plus screen, double/triple click selects words/logical lines, and dragging extends by whole units across soft wraps. CPU-only regression tests cover boundaries, scrollback, invalidation and the explicit 1 MiB copy limit; browser visual verification remains pending without Emscripten. See `docs/SELECTION.md`.
- [x] Bug: rows duplicated when the window is resized while a full-screen TUI such as Claude Code is running. Cause: a height shrink always pushed the top rows into the scrollback, even when the rows below the cursor were blank, so the program's redraw after SIGWINCH (cursor up, erase, reprint) printed its frame a second time. `TerminalModel::resize` now drops blank rows below the cursor first (xterm/VTE behavior); regression tests in `test/terminaloperations_test.cpp`. Remaining, not part of this fix: no reflow on width change, growth does not pull scrollback down, and a real recorded session has not been compared.
- [x] fast-json (our own library, `lib/fast-json`) bugs found by the server config loader: the tokenizer hanging on negative numbers, truncated files and missing commas being accepted, `1.5` read as `1`. Fixed in the library with regression tests (plus valid arrays throwing `vector::_M_range_check` and non-inline specializations); the loader's workaround is removed. A trailing comma before `]` or `}` stays accepted, by decision.
- [x] Inconsistent blank cells (fixed: screens start with default-color cells): cells never written have an RGB-black background, while cells cleared through the parser use the default color. `blankRow` in `TerminalModel::resized` accepts both; unify them so themes and palette changes treat blank cells the same.
- [ ] Resize follow-ups from the duplication fix: growing the window adds blank rows at the bottom, where xterm and VTE pull scrollback down; width changes do not reflow (matterm follows xterm, VTE reflows). If duplicated rows persist under Claude Code after narrowing, take a raw PTY recording during a resize (docs/PTY.md) before changing anything.
- [x] Smooth cursor animation (optional, `--smooth-cursor` / `?smooth[=ms]`; `CursorGlide` in src/core): glide the cursor between positions instead of jumping (render-side interpolation only; the model keeps exact cell positions; must be optional and cheap when idle).
- [ ] Typewriter output animation: optionally reveal newly arrived text one character at a time, only under defined conditions (for example small bursts or an explicit mode, never during bulk output). The delay schedule lives in its own buffer, separate from `TerminalModel`, so the model always holds the true state and the renderer decides what is revealed yet.
- [x] Ctrl+left click opens detected HTTP(S) links, with wrapped-row CPU detection/security tests, a 2048-byte cap, trimmed prose punctuation and a presentation-only hover underline/title. Native uses `SDL_OpenURL`, browser uses `window.open` with `noopener,noreferrer`; application mouse reporting takes precedence unless Shift is also held. No shell or OSC 8 support. Conservative ASCII URI policy and verification limits: `docs/LINKS.md` (browser/real-opener checks pending without Emscripten).
- [ ] Design and prototype a borderless SDL window with a custom title bar. Budget substantial GUI design and polish before rollout; keep native decorations as a fallback.
- [ ] Reproduce prompt and Pi panel color reports; see [Interaction and Compatibility](docs/INTERACTION_AND_COMPATIBILITY.md).
- [ ] Golden transcripts from real programs, fuzz/property tests, and compatibility fixtures.
  - [x] Bounded seeded codec/session, VT chunking/model-invariant, and server auth/config property tests in the normal suite; reject malformed JSON string encodings found by them. Recorded golden transcripts, coverage-guided fuzzing and broader compatibility fixtures remain open.
- [x] Use optional Fontconfig system font fallback on Linux for missing glyphs, including Braille.
- [x] Verify the Fontconfig-enabled build and Braille fallback with installed system fonts.
- [x] Reproduce and fix an SDL_ttf `Text has zero width` exit: U+00AD soft hyphen has nonempty glyph metrics but zero rendered text width with DejaVu/SDL_ttf 2.20.1. The atlas now measures the selected font/style before rasterizing and caches zero-width text as an empty glyph, preserving advance and other error propagation. Regression tests cover styles, caching and atlas failures. The exact trigger of the original session is unconfirmed; see `docs/tasks/zero-width-glyph.md`.
- [ ] Wide cells, combining clusters, shaping, and system font fallback on other platforms.

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
- [x] Close the application window when its PTY session finishes.
- [x] Add display-independent `--help`/`-h`, document `--smooth-cursor` versus smooth history navigation, and reject silently ineffective cursor-animation options in non-PTY modes (handoff: `ONGOING.md`).
- [x] Opt-in raw PTY output recording for reproducible rendering diagnostics.
- [x] Optional integration fixtures for a real Python curses screen (DEC borders/input) and top.
- [ ] Broader full-screen/editor transcripts and interactive compatibility testing.
- [x] Keep dangerous OSC actions disabled; any future opt-in requires a security policy.

## Future Direction: Session Multiplexing (Do Not Implement Yet)

- [ ] Decouple terminal session ownership (PTY, model, and scrollback) from SDL windows and views so a live session can be moved without restarting its process.
- [ ] Support multiple independent terminal sessions in one window. Prototype tabs first; evaluate split panes and focus/keyboard navigation separately.
- [ ] Move a session to an existing or new window, preserving its process, screen, and scrollback. Define which view owns input and PTY sizing during a move.
- [ ] Decide what closing the last view means before implementing detach/reattach or persistence across application restarts. Start with moves between windows in one matterm process, not a full tmux replacement.

## Reported Scrolling Issues and Palette Design

- [ ] Reproduce and eliminate the reported blank area above the first row in
  both primary and alternate mode; check initial padding, viewport bounds, and
  fractional clipping separately rather than introducing intentional overscroll.
- [ ] Empty terminal content must not allow scrolling. Distinguish allocated blank
  screen rows from retained scrollable history; test fresh, cleared, resized,
  alternate, and post-reset states. Do not discard intentional blank output lines.
- [ ] Decide explicitly whether alternate-mode wheel input should scroll history,
  be ignored, or encode application input; currently it sends arrow keys.
- [x] Retain default/indexed/RGB color identity in terminal cells and resolve via caller-owned Palette during rendering.
- [ ] Define palette ownership, 16/256-color lookup, default foreground/background,
  theme changes affecting existing cells, inverse/faint rendering, and future OSC
  palette commands. Keep cell layout compact and the API backend-independent.
- [ ] Add palette and scroll-boundary regression tests before changing behavior.

Palette representation/rendering is implemented. Scroll fixes and persistent settings are still pending.

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

- [ ] Remote terminal (design: [Web Terminal](docs/WEB_TERMINAL.md)): a server owns a PTY/process, while a browser owns the parser,
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

## Configuration, Measurements, and Independent Work

- [x] Document independent-repo task/handoff rules and candidate parallel tasks in docs/PARALLEL_WORK.md.
- [x] Inspect the local matperf API and document usage/caveats in its repository.
- [x] Record XDG text configuration, benchmark completion semantics, and deferred pipeline design in docs/SETTINGS_AND_PERFORMANCE.md.
- [x] Implement approved 12-byte color-reference layout and dark palette before parallel color consumers (docs/COLORS.md).
- [x] Allow a startup font-size override in the terminal application (`--font-size PIXELS`, default 18).
- [ ] Select a text configuration parser/schema and implement typed settings, validation, and atomic saving.
- [x] Add shared deterministic workloads and first internal parser/model Release measurements (docs/BENCHMARKS.md).
- [x] Add POSIX stdout benchmark with separate write/DSR timings, bounded reply handling, and termios restoration tests.
- [ ] Collect comparable external samples in matterm and other emulators on a real display before optimizing.
- [ ] Review matperf concurrency before enabling optional coarse tracing.
- [ ] Trial one isolated parallel task after agreeing on a base commit/API.

## Bundled Font

- [x] Pin unmodified JetBrains Mono Regular v2.304 and OFL 1.1 license by revision/hash.
- [x] Configure-time verified download, reusable cache, and explicit offline/opt-out options.
- [x] Relocatable assets/fonts next to all three executables, including installed license.
- [x] Optional font CLI override and default-font smoke tests from a different directory.
- [ ] Browser preloading and platform/distribution-specific packaging if needed.

## References

- Xterm control sequence reference: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- Linux tty ioctls: https://man7.org/linux/man-pages/man4/tty_ioctl.4.html

These describe much more functionality than matterm currently implements.
