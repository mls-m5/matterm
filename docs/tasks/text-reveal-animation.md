# Task: animated text reveal (demo first, then the regular terminal)

Owner request: text should not just pop onto the screen; it should be possible to
animate how it appears, and to enable that in the regular terminal too.

## Phase 1: the demo (agent3, part of fakeos M4)

fakeos controls its own output, so it can pace it:

- Program output is released through a rate limiter (characters or lines per
  second, named constants). Any key speeds up or skips. Typed input is never delayed.
- Smooth transition from the intro to the terminal (background fade).
- Smooth cursor on by default in `matterm-demo` and the static web demo.
- Pure C++ in `src/fakeos`, deterministic, tested with the fake clock.

## Phase 2: renderer-level reveal (regular terminal, opt-in)

A real shell and real programs cannot be slowed down, so this is a visual effect
only. The terminal model and the PTY are untouched.

- Cells whose content changed fade in (alpha ramp, ~100-200 ms), optionally with a
  short left-to-right stagger per line, driven by the same frame/idle logic as the
  smooth cursor (animation keeps frames active; idle throttling still applies).
- Opt-in like `--smooth-cursor`: `--reveal-animation` (native) and a web setting;
  off by default. Must not cost idle CPU or add input latency (typed echo is shown
  at once, only bulk output is staggered).
- Lives in the renderer (`SdlBackend`/frame signature), tests with the dummy video
  driver and a fake clock.

## Open questions

- Per character, per line or per changed block? Start with per line fade + stagger.
- Interaction with scrolling (scrolled-in rows should not re-animate) and resize.
