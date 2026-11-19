# Task: rows duplicate when the window is resized under a TUI

Assignment for an independent agent in its own worktree (`.tmp/agent3`, branch
`agent3-resize`). Read first: Agents.md, docs/PARALLEL_WORK.md, DESIGN.md (the
terminal model, screen and scrollback ownership), `docs/VT.md`, and the model and
memory code: `src/terminal/terminalmodel.*`, `src/core/terminalmemory.*`,
`src/core/scrollviewport.*`, and the tests in `test/terminalmodel_test.cpp`.

## Observation (from the owner)

In the native terminal, running Claude Code (a full-screen TUI, the Ink
library) and then resizing the window makes some rows appear duplicated. It is
not related to pasting. Nobody has a reproduction in the test suite yet.

## Goal

Find the cause with a failing test, fix it in the model, and keep behavior that
other terminals share (xterm, VTE) in mind.

## Method

1. **Reproduce in a CPU-only test first.** Build the screen with `TerminalModel` +
   `VtParser`, fill the screen and some scrollback, resize narrower and wider
   (and shorter and taller), and check that no row is duplicated or lost: compare
   the visible text and the scrollback text before and after a resize that should
   preserve content.
2. **Model what such a TUI does.** Ink and similar programs redraw by moving the
   cursor up N rows (`CSI nA`), erasing (`CSI 2K`, `CSI J`) and rewriting, sometimes
   with an unterminated last line and in the primary screen (not the alternate
   screen). On resize they redraw again after SIGWINCH, based on their own idea of
   how many rows they printed. Write the test as a sequence of VT bytes (print a
   frame of M lines, resize, run the cursor-up/erase/redraw sequence, resize
   again) and assert the final screen has each line once.
3. Check hypotheses before changing code: how `TerminalModel::resize` moves rows
   between the screen and scrollback when the height shrinks or grows; whether
   soft-wrapped rows are re-joined on width change (reflow) and what the cursor
   position does; saved cursor; the alternate screen. Compare with what xterm
   (no reflow, rows pushed to scrollback on shrink) and VTE/GNOME Terminal (reflow)
   do, and say in the handoff which behavior matterm now follows and why.
4. Fix it, keeping all existing tests passing.

## Scope

`src/terminal/terminalmodel.*`, `src/core/terminalmemory.*`,
`src/core/scrollviewport.*` and their tests, plus `docs/VT.md` or DESIGN.md if
resize semantics are documented there. If the fix needs a decision with
trade-offs (for example whether to reflow), implement the least invasive correct
option, document the alternative and stop short of a redesign.

## Out of scope

Renderer and SDL code, the PTY adapter, the web client, the protocol and server,
new VT features.

## Acceptance

- At least one test that fails on the current code and passes after the fix, and
  a note on how you know it matches the reported symptom.
- The full existing suite passes in the default configuration; clang-format applied;
  no build products committed.
- Update the ROADMAP.md item for this bug (check it off or state what remains).

## Handoff

As in docs/PARALLEL_WORK.md. If you cannot reproduce the duplication with a
CPU-only test, say so, list what you tried, and describe the smallest piece of
information from a real session that would help (for example a raw PTY recording:
the native app can record output on request, see docs/PTY.md).
