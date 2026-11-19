# Task: text selection that works like other terminals

Assignment for an independent agent in its own worktree (`.tmp/agent3`, new
branch `agent3-selection` from `main`). Read first: Agents.md,
docs/PARALLEL_WORK.md, ONGOING.md, the ROADMAP.md item "Selection with the
keyboard modifiers users expect", `src/frontend/sdlinput.h` (`Selection`,
`mouseCell`, shared by the native app and the browser client),
`TerminalModel::copyText` / `selectionEpoch` in `src/terminal/terminalmodel.*`,
and the selection and mouse code in `app/ptyfrontend.cpp` and `web/main.cpp`.

## Problem (from the owner)

Shift+click does nothing, so selecting a large region means dragging across the
window, which makes "select everything" very hard.

## Scope

1. **Shift+click extends** the current selection: it moves the end of the
   selection (keeping the anchor) to the clicked cell, and shift+drag continues
   from there. Without a selection, shift+click starts one. While an application
   has mouse reporting on, Shift already hands the mouse back to local selection
   (see `ptyfrontend.cpp`); keep that rule.
2. **Select all**: Ctrl+Shift+A selects the whole retained history plus screen
   (bounded by the same 1 MiB copy limit; copy must still report a clear failure
   when it is exceeded).
3. **Double click selects a word, triple click a line**, with a documented word
   definition (alphanumerics plus `_` and a short list of path/URL characters
   such as `-./:@~%+=?&#`), across soft-wrapped rows as the existing copy does.
   Dragging after the double/triple click extends by words/lines.
4. Put the pure logic (extending, word and line boundaries, select all) in the
   shared header or a new CPU-only file under `src/frontend/` or `src/terminal/`
   with unit tests that need no SDL; both front ends use it. Keep SDL event
   plumbing thin in `app/ptyfrontend.cpp` and `web/main.cpp`.

## Out of scope

Right-click menus, drag auto-scroll, rectangular (block) selection, URL
detection (another task), changes to the protocol and server, the renderer's
selection colors.

## Owned and shared paths

Owned: `src/frontend/**`, new selection tests. Shared, edit minimally and report:
`app/ptyfrontend.cpp`, `web/main.cpp` (only the mouse/selection block; the
integrator edits other parts of `web/main.cpp`), `src/terminal/terminalmodel.*`
only if a model helper is truly needed (do not change resize behavior).

## Acceptance

- Unit tests for each behavior, including wrapped rows, wide ranges across
  scrollback, and selections invalidated by `selectionEpoch`.
- The default suite passes; the browser build compiles when `emcmake` exists (if
  not, say so). A headless-browser check for double click, triple click and
  shift+click is welcome (see `web/tools/`), if the toolchain is available.
- clang-format applied; no build products committed. Update the ROADMAP item.

## Handoff

As in docs/PARALLEL_WORK.md. Do not push.
