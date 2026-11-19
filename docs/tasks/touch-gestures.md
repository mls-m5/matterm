# Task: touch scrolling and pinch zoom in the browser client

Assignment for an independent agent in its own worktree (`.tmp/agent3`, new branch
`agent3-touch` from `main`). Read first: Agents.md, docs/PARALLEL_WORK.md,
web/README.md (phones and tablets), `web/main.cpp` (`handleEvent`, `handleWheel`,
`fontPixels`, `frame`), `web/login.js` (`fitCanvas`, `setupTouchKeyboard`) and
docs/SELECTION.md. The owner will test on an Android phone.

## Goal

On touch devices the terminal can be scrolled with a one-finger drag and the font
size changed with a two-finger pinch, without breaking selection, the on-screen
keyboard or application mouse reporting.

## Scope

1. **One-finger vertical drag scrolls** the scrollback with the same model call
   the wheel uses (`handleWheel` path), with momentum optional (keep it simple: a
   short inertia is fine, none is acceptable). A short tap stays a click
   (focus, show the keyboard); a drag must not start a text selection and must
   not move focus or toggle the keyboard.
2. **Long press then drag selects text** (select a word on long press, extend by
   dragging). If this cannot be done robustly without a lot of code, document the
   limitation and keep the keybar/copy route.
3. **Two-finger pinch changes the font size** (steps of 1 px around the current
   size, clamped to a sane range, for example 8 to 40 CSS px times
   devicePixelRatio) and re-fits the grid, notifying the server through the
   existing resize path. Persist the size in `localStorage` (wrapped in
   try/catch) and restore it. Ctrl+wheel on desktop may do the same.
4. **Application mouse reporting** (vim, less with mouse on): a drag keeps being
   reported as the wheel/mouse events the application expects; follow the
   existing rule that Shift hands the mouse to local handling. Document what a
   touch drag does in each mode (alternate screen: arrow keys as the wheel does
   today, unless the existing code decides otherwise).
5. **Do not break** the on-screen keyboard behavior, the keybar, Ctrl+click links
   and the idle throttling: touch events must wake the loop (SDL events already
   do; verify) and must not make idle frames expensive.
6. Tests: extend the Playwright tools in `web/tools/` (see `e2e-touch.js`; headless
   Chromium can emulate touch with `hasTouch: true` and CDP
   `Input.dispatchTouchEvent`) for drag scroll, tap, and pinch. Add a case to
   `web/tools/run-e2e.sh`. Pure logic (gesture classification, font step clamping)
   belongs in a header under `src/frontend/` with a unit test in `test/`.

## Out of scope

Server, protocol, the native app, session multiplexing, changing the palette.

## Owned and shared paths

Owned: new files under `src/frontend/` and tests, `web/tools/*`. Shared with the
integrator, edit and report: `web/main.cpp`, `web/login.js`, `web/style.css`,
`web/README.md`.

## Acceptance

Full ctest suite passes (`-DMATTERM_BUILD_SERVER=ON`); the web build
(`emcmake cmake -S web -B <dir> -DMATTERM_FONT=...`) and `run-e2e.sh` pass; commits
are small with the tests first where practical; a short report lists behavior per
mode and anything unverified on a real phone. Do not push.
