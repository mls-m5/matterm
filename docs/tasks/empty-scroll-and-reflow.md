# Task: no scrolling of empty content, blank area above the first row, resize follow-ups

Assignment for an independent agent in its own worktree (`.tmp/agent2`, new branch
`agent2-scroll` from `main`). Read first: Agents.md, docs/PARALLEL_WORK.md,
ROADMAP.md ("Reported Scrolling Issues and Palette Design" and the resize
follow-ups), `src/terminal/terminalmodel.{h,cpp}`, `src/core` viewport code and the
existing tests in `test/terminaloperations_test.cpp` and `test/terminalmodel_test.cpp`.

## Goal, in this order (commit each separately, tests first)

1. **Empty terminal content must not allow scrolling.** Reproduce first in tests:
   fresh model, after clear (`ESC[2J`, `ESC[3J`), after `reset`, after resize,
   primary and alternate screens. Distinguish allocated blank screen rows from
   retained scrollable history; do not discard intentional blank output lines.
   The wheel/scrollbar must have nothing to scroll when there is no history, and
   the scrollbar must not show.
2. **The reported blank area above the first row**: reproduce (initial padding,
   viewport bounds, fractional clipping, primary and alternate) and fix without
   introducing intentional overscroll. If it cannot be reproduced in a test, say so
   with what you tried.
3. **Growing the window pulls scrollback down** (xterm/VTE behavior) instead of
   adding blank rows at the bottom, in the primary screen only; tests for
   exact row content before and after shrink-then-grow.
4. **Reflow on width change** is a stretch goal: write a short design note in
   `docs/` first (what wraps, soft-wrap flags, cursor and selection positions,
   alternate screen untouched, cost on large scrollback) and implement only if it
   stays small and fully tested. Otherwise leave it documented.
5. Decide and document alternate-screen wheel behavior (currently arrow keys);
   change only if the owner's notes in ROADMAP.md say so.

## Out of scope

Server, protocol, browser input handling (`web/`), rendering colors, session
multiplexing.

## Owned and shared paths

Owned: `src/terminal/**`, the viewport code in `src/core`, their tests. Shared:
`ROADMAP.md` (tick your items only). Do not edit `web/` or `src/server/`.

## Acceptance

Full ctest suite passes (also with `-DMATTERM_BUILD_SERVER=ON`); the fuzz tests
still pass; each fix has a test that failed before; a short report lists what was
changed, what could not be reproduced and anything unverified. Do not push.
