# Scrollback, viewport bounds, and resize follow-ups

## Implemented policy

Allocated live screen rows are not scrollback. `historySize()` counts only saved
rows above the live screen. With no saved rows, wheel-style calls to `scrollBy`
and `scrollToOldest` have zero travel, and both the viewport scrollbar and the
terminal frame's scrollbar are hidden. The alternate model has no saved rows.

Do not infer history from non-space characters: deliberately output blank lines
are real history. `CSI 2 J` erases the display, not saved rows, so a blank display
can still have scrollable history. `CSI 3 J` / `TerminalModel::eraseScrollback()`
discards saved primary rows without clearing live cells or changing cursor,
rendition, saved cursor, deferred wrap, margins or modes. It snaps the viewport
to its now history-free tail and invalidates selection coordinates through the
selection epoch. On the alternate screen it is a no-op; hidden primary history
is not erased. RIS/reset still resets both screens.

Height shrink keeps the existing policy: discard blank rows below the cursor
first, then move top primary rows into saved history. On height growth, reveal
up to the added height from the newest saved primary rows, in chronological
order. Only the remaining added height is filled with blank rows. The cursor
and saved cursor move down with the revealed rows. Rows already evicted by the
history limit cannot be recovered. Alternate growth only appends blanks, and
resizing while alternate is active also correctly resizes the inactive primary.

A historical reader's target and displayed offsets each decrease by the number
of revealed rows. This keeps the same first row/fraction visible when possible;
a view now covered by the live screen clamps to the tail. A tail-following view
stays at the tail. Resize still resets margins/origin as before and invalidates
selection coordinates. Width changes still clip/pad, without reflow.

The saved-line erase rebuilds the primary screen before replacement, retaining
the allocation-failure guarantee of resize. Like existing resize, it allocates
bounded-capacity storage and temporarily retains the old screen; it is not an
allocation-free operation on very large scrollback.

## Reported gap above the first row: not reproduced

CPU tests cover initial top-left output, primary/alternate screens, full history
and ring eviction, oldest/tail bounds, tiny and half-row deltas, animation steps,
large clamped deltas, and return to the tail. Frames start with the expected
stored row (no inserted leading blank), have offsets in `(-cellHeight, 0]`, and
supply enough rows to cover the text height. The oldest position has row zero
and pixel offset zero. Empty/cleared/resized screens also remain at zero offset.

These tests did **not** reproduce the reported top gap. No speculative padding
or intentional overscroll was added, and the roadmap issue stays open. This does
not validate physical SDL clipping, browser canvas/CSS sizing, high-DPI rounding,
or a specific application's deliberately printed leading blank rows. A raw PTY
recording plus viewport/window/cell geometry and a screenshot at the failure is
still needed before changing policy in those layers.

Tests: `scrollviewport_test`, `terminalmodel_test`,
`terminaloperations_test`; existing `vtfuzz_test` remains a chunking/invariant
guard. The new regression tests failed before the corresponding scrollbar, ED3,
and growth fixes; the top-gap probes pass without a production change.

## Alternate-screen wheel policy (retained, not changed)

Keep the existing terminal application's fallback: when application mouse
tracking is active and Shift is not held, wheel input is encoded as application
mouse events. Otherwise, wheel input on alternate sends Up/Down keys using the
current input modes; it does not reveal primary scrollback. Shift bypasses mouse
reporting, but does not create alternate history: the existing arrow-key fallback
still applies. Primary fallback navigates saved history.

This is frontend input policy, not behavior of the CPU-only model. A library
client or the standalone terminal-model demo that calls `scrollBy` directly on
alternate gets no travel and no scrollbar, not synthesized keys. No native or
browser input handler was changed in this task. Touch gesture handling is a
separate task; this document does not promise that every gesture uses this path.

## Width reflow design note — deferred

Reflow is not small enough to implement safely as part of the height fix. The
following contract should be agreed before implementation:

1. **Logical lines.** Join retained primary rows only across their `softWrap`
   flags. Hard newline boundaries, intentionally empty rows, cell styles and
   logical colors must survive. Decide how to distinguish meaningful trailing
   spaces from padding: the current row representation has no written-extent
   metadata. Do not silently trim user text or join hard lines.
2. **Layout.** Lay each primary logical line into the new width and regenerate
   soft-wrap flags. Handle the boundary where the oldest retained row is the
   middle of an evicted logical line. Apply the saved-physical-row limit only
   after choosing the new live screen; trim oldest physical rows, never allocate
   an unbounded intermediate logical line/string.
3. **Positions.** Map the active cursor and saved cursor through a logical-line
   index plus cell offset, including deferred wrap at the right edge. Define
   behavior for erased cells, positions after meaningful text, and lines trimmed
   by capacity. Preserve input/rendition state while resize resets margins/origin
   as today. Preserve a historical reader's logical anchor rather than blindly
   preserving a physical offset.
4. **Selections and links.** Current frontend positions use absolute physical
   row IDs plus a selection epoch. Initially invalidate selection/link hit data
   on reflow (as on resize now); retaining a selection would need explicit
   logical-coordinate mapping, not reusing old row IDs.
5. **Alternate screen.** Leave it on clip/pad semantics: full-screen applications
   redraw after SIGWINCH. Do not reflow its rows or mix it with primary history.
6. **Cost and failure.** Reflow is O(retained cells), potentially millions of
   cells per interactive width change, with extra storage needed for rollback.
   Keep ring/history bounds, check multiplication/size overflow before allocation,
   preserve the old model on failure, and benchmark resize bursts on large
   histories before deciding whether coalescing is needed.

Required tests include repeated narrow/wide cycles, full-width deferred wrap,
partial/evicted logical lines, intentional spaces and blank lines, RGB/indexed/
default styles, saved cursors, historic viewport anchors, selection invalidation,
alternate isolation, capacity eviction, and allocation/overflow failures. A
recorded full-screen transcript should accompany CPU properties before claiming
compatibility. No reflow implementation or new policy for trailing spaces lands
in this task.

## Reflow: what is still open (checked again, not started)

Not bounded enough to implement now. Decisions and work that remain, in order:

1. **Written extent.** Rows have no "last written column", so trailing spaces
   that the user typed cannot be told from padding. Either add a per-row extent
   (set by writes, cleared by erase) or settle for "trailing default-styled blanks
   are padding". Everything else depends on this; it also decides how a full
   line that was exactly the old width is joined.
2. **Wrap flags.** Check that every path that wraps (autowrap, deferred wrap,
   insert/delete, scroll regions, ED/EL) sets and clears `softWrap` correctly.
   Today nothing relies on it, so errors would first show up as joined hard lines.
3. **Algorithm** over primary rows only (logical lines, new layout, trimming to
   the saved-row limit afterwards), with the old model kept until the new one is
   complete.
4. **Positions:** cursor, saved cursor, deferred wrap, the historical viewport
   anchor; selection and link data are invalidated, as in any resize.
5. **Cost:** a benchmark of width-change bursts on a full history, then decide
   about coalescing resizes.
6. **Tests** from the list above, and a recorded full-screen session as a guard.

The scroll-target snap in `ScrollViewport::scrollBy` (a drag and its reverse end
exactly at the tail) is unrelated to this and is covered in `scrollviewport_test`.

## Owner's requirement (2026-11-21): narrowing must not lose information

Today a width decrease clips: cells right of the new width are dropped, so the text
is gone even if the window is widened again (matterm follows xterm here; VTE and
several other emulators reflow soft-wrapped lines instead, which I have not
re-checked for each of them). The owner wants a solution in the long run where
information does not disappear when the width is reduced.

This matters more with tabs and panes (`docs/TABS_PLAN.md`): dragging a divider
changes a pane's width many times, so clipping would lose text often, not only
when a window is resized once.

Two approaches, not exclusive (decide before implementing; nothing is started):

1. **Reflow** (the design above): soft-wrapped lines are joined and laid out again at
   the new width. Nothing is lost for text that wrapped by itself; hard lines that
   are longer than the new width wrap on screen and are restored when widened.
2. **Keep what was clipped:** every row remembers the cells beyond the current
   width (a hidden overflow, bounded by the old width and the history limit) and a
   widening restores them. Simpler than reflow and lossless for round trips
   (narrow, then wide again), but narrowed text is cut on screen, not wrapped.
   It also needs a rule for what happens when the program writes into the row
   while the overflow exists (drop the overflow of that row).

The alternate screen stays out of both: full-screen programs redraw after SIGWINCH.
Resize bursts need coalescing either way (a divider drag; see `TABS_PLAN.md`
section 6), and the written-extent question (trailing spaces) above applies to both.

## Design: narrowing without loss (2026-11-21, proposal, nothing implemented)

Written against the code as it is: `TerminalMemory` (ring of fixed-width rows, `columns_`
wide, `resizeColumns` copies and clips), `TerminalModel::Impl::Screen` (a
`TerminalMemory`, a parallel `std::deque<bool> softWrap`, `firstRow` as the absolute id
of the oldest retained row), and `Impl::resized()`, which builds a *new* screen by
appending every old row (so `append` clips or pads it) and sets `softWrap` only if the
width did not change (`width == columns && old.softWrap[y]`). A row has no written
extent: a cell never written and a blank cell look the same. Selection and links use
absolute row ids plus `selectionEpoch`, bumped by every resize. The alternate screen
is a separate screen without history.

### What each approach needs, measured against that representation

| | Reflow | Keep clipped cells |
|---|---|---|
| Storage | Rows change number and identity: every row id after the first joined line moves; `firstRow`, `softWrap`, ring contents are rebuilt | Rows keep their ids; only the row width in memory differs from the visible width |
| Needs a written extent | Yes (trailing blanks vs text) and correct `softWrap` on every wrapping path (nothing relies on it today) | No: cells are kept as they are, blanks included |
| Cursor, saved cursor, viewport anchor, deferred wrap | Must be mapped through logical-line offsets (hard) | Unchanged rules (clamp the column as today) |
| Shell redraw after SIGWINCH (readline, zsh) | Rows above the cursor change under the shell, which counts rows by its own width: known prompt duplication and artifacts in emulators that reflow | Same screen as xterm after narrowing, so shells behave as they do now |
| Time | O(retained cells) with a new allocation per width change, at every divider event | O(rows) to mark overflow, no copying when the stored width is enough |
| Result when narrowed | Text wraps on screen; readable | Text is cut on screen; restored when widened again |
| Lossless for narrow, then wide | Mostly (hard lines wider than the pane wrap and unwrap) | Exactly, for every cell not rewritten meanwhile |
| Alternate screen | Out | Out |

The dividers case (a pane narrowed during a drag, widened again a moment later) is a
round trip, which keep-clipped does exactly and cheaply. What it does not give is a
readable narrow history; that is the one thing reflow does better, and it can be had
later for the history region only (below).

### Recommendation

1. **Step 1: keep clipped cells** on the primary screen (live rows and history). Small,
   compatible with xterm, bounded memory, fast enough for a drag.
2. **Step 2, optional and later: reflow the saved history only.** Rows above the live
   screen are never redrawn by programs, so the shell-redraw problem does not apply
   to them; the live screen stays keep-clipped. Needs the written-extent decision and
   correct `softWrap`; not designed further here.

### Step 1, exact changes

Memory: a row is stored at `storedColumns_ >= columns_` cells; the visible width is
`columns_`. Rows are already handed out as `FStringView(slot, columns_)`, so views and
frames do not change.

1. `src/core/terminalmemory.h/.cpp`
   - Add `storedColumns_`, `maxStoredColumns` (constructor argument, default 512, see
     "bounds"), and per-row `overflow` counts (`std::deque<std::uint16_t>`, cells at or
     right of `columns_` that differ from the fill, 0 for most rows).
   - `resizeColumns(columns, fill)`: if `columns <= storedColumns_`, only set
     `columns_` and recompute the overflow counts (O(rows x (stored - columns)), no
     copying); otherwise allocate `min(columns, ...)` stored columns, copy whole stored
     rows, fill the new cells with `fill`. The widening case never loses cells that are
     still stored; the narrowing case beyond `maxStoredColumns` drops the cells right of
     the cap (document: a 2000-column window narrowed to 100 keeps 512).
   - `storedRow(index)` (stored width view, for rebuilding) and
     `append(FStringView stored, fill)` taking a stored row, so a rebuilt screen keeps
     the overflow; plain `append` pads the whole stored width with `fill`.
   - `dropOverflow(index, fill)`: fill `[columns_, storedColumns_)` of one row, set its
     count to 0.
2. `src/terminal/terminalmodel.cpp`
   - `Impl::resized()`: for the primary screen copy `storedRow(y)` (not `row(y)`) and
     create the new memory with `max(old stored, width)` capped; for the alternate
     screen keep today's clipping (`stored == visible`).
   - Keep `softWrap` through width changes (the flag describes how the line was
     written, clipping does not change it): drop the `width == columns` condition for
     the primary screen. `copyText` and word/line selection still read visible cells
     only; document that a clipped row copies as what is visible.
   - Every mutation of a row calls `dropOverflow` first, so a row written after
     narrowing never mixes old hidden text with new visible text: `fillRow` (line 127),
     `copyRow` (139, destination row), `shiftRows` (147), `write` (the cell store
     near 438), `eraseCharacters`, `insertCharacters`, `deleteCharacters`
     (477-520), `insertLines`/`deleteLines`/scrolls through `fillRow`/`copyRow`. Rows
     created by scrolling are new rows and have no overflow.
   - `eraseScrollback()` and `reset()` go through `resized()`/the constructor and need
     no extra work; `resize()` bumps `selectionEpoch` as today.
3. Config: `TerminalViewConfig`/`TerminalModel` constructor get `keptColumns`
   (default 512; 0 means "clip as today").

### Bounds and costs (state them in the code comments)

Memory is history rows x `max(columns, keptColumns)` x `sizeof(FChar)` per primary
screen (check `sizeof(FChar)`; at 12 bytes, 1000 rows x 512 = about 6 MB per pane,
about 50 MB for eight panes); the default can be lowered per pane or per build. It
only grows when a window is wider than before, and never shrinks while the screen
lives (`reset()` and `eraseScrollback()` rebuild it). A resize costs O(rows) with no
allocation when narrowing, and one allocation of all rows when the stored width
grows. Resize bursts still need the 40 ms throttle of `WorkspaceView`.

### Tests (all CPU-only, `terminalmodel_test` or a new `terminalresize_test`)

- Round trip: random content with colors and attributes in screen and history; narrow
  to every width from 1 to W-1, widen back to W; all visible cells equal the original
  (seeded property test, 200 rounds, also with several steps in between).
- Cells beyond the cap are lost and the loss is exactly the documented amount;
  `keptColumns == 0` equals today's behavior byte for byte (existing tests unchanged).
- A write, erase (`EL`, `ED`, `ECH`), `ICH`, `DCH`, `IL`, `DL`, scroll or tab after
  narrowing drops the overflow of the touched rows only; untouched rows still restore;
  a widened row never shows old text to the right of new text.
- New rows created by scrolling or by height growth have no overflow; history
  eviction (the ring wraps) keeps the overflow counts aligned with the rows.
- Height and width changed together; resize while the alternate screen is active
  (the alternate screen clips, the primary underneath keeps its overflow);
  `eraseScrollback()` keeps the live rows' overflow and drops the history's.
- Cursor, saved cursor, deferred wrap and viewport anchors behave as in today's
  resize tests (add the width cases to each).
- Soft-wrap flags survive a narrow/wide round trip and word/line selection joins the
  same rows afterwards; selection epoch changes on every resize.
- A bound test: stored cells never exceed `rows x max(columns, keptColumns)`; a
  1000-step random resize sequence does not grow allocation beyond that.
- A resize burst benchmark (`bench/`, width changes on a full history) before and after,
  with the numbers recorded here.

### Open questions

- The default `keptColumns` (512 covers wide terminals, costs memory for every pane).
- Wide characters (not supported in `FChar` yet): a clip boundary must never split one;
  decide with the wide-cell work.
- Whether a clipped row should show a marker (for example a dim `>` in the last column)
  so the user knows text is hidden; a rendering decision for the owner.

## Owner's answer (2026-11-22): reflow is the goal, steady-state speed matters most

- The goal in the future is GNOME Terminal behavior: soft-wrapped lines are remembered
  and the text reflows when the size changes. "Keep the clipped cells" (prototype
  `agent2-keepclipped`, ca6e270, not merged) is at most a stopgap.
- Priority of performance: first the cost when the size does **not** change (writing,
  scrolling, drawing, memory per row), then the cost of a resize. A resize may take
  longer (milliseconds to a short pause) if the steady state stays as fast as today.
- So before any of the two is merged: measure the steady state with and without the
  change (write throughput, scrolling a full history, memory per row, the lazy-drop
  hook in `TerminalMemory::at()` on the write path) and compare with `main`.
- The soft-wrap flag fix of the prototype (flags survive width changes on the primary
  screen) is a prerequisite of reflow and can be taken on its own.

## Wrapped-line marker (owner, 2026-11-22; design only, nothing implemented)

Soft-wrapped lines get a small visible mark at the end of each row that continues on the next
one, so a long command or log line can be told from separate lines (and so the user sees what
reflow will join). The owner suggested a small right-pointing triangle or another glyph that one
rarely types.

**Where it is drawn.** In the renderer, at the end of every visible row whose
`softWrap` flag is set (the row continues below). It is not cell text: the last cell
keeps its character, selection, copy and search never see the mark, and copying a wrapped line
still joins it without a newline (what `softWrap` already means for copy). A wrapped row is full by definition, so its last
column always holds a character and the mark cannot go there. Two choices:
1. **Reserve one column** for the mark (the terminal is one column narrower than the pane).
   Wrong: programs would see a different width than the pane.
2. **Draw in the right gutter** (recommended): the pane already reserves a scrollbar strip on
   its right; the mark sits in the pixels between the last cell and the strip, vertically
   centred on the row, as a small shape drawn by the backend (a triangle of ~40% of the cell
   height in a dim foreground), not as a font glyph. No cell is covered, no width changes,
   and no glyph has to exist in the bundled font, so no cmap test is needed. If the gutter
   has no room (very narrow panes, compact mode) the mark is simply not drawn.

A glyph variant (U+21B5 or U+25B8 over the last cell, dimmed) stays possible as the fallback
for the web port if the gutter is not available there; it would need the cmap test the palette
button has, and it hides the last character, so it is the less good choice.

**Interface.** `RenderFrame` gets `std::vector<bool> wrapMarks` (one entry per visible row;
empty means none). The model fills it from `softWrap` when it builds the frame (it already
knows the flag per row), the backend draws it. Frame signatures (`windowSignature`) include the
flags so a change redraws. Alternate-screen rows never have the flag set.

**Setting.** `wrap_marker = on|off` (default on once reflow exists; default off before, since
without reflow the mark would promise something that does not happen). Colour: the dim
foreground, which is the same as the scrollbar thumb at rest.

**Tests.** `terminalmodel_test`: the frame marks exactly the rows that continue (a 25-character
line at width 10 gives rows 0 and 1 marked, row 2 not; an erase or a hard newline clears the
mark). `sdlbackend` pixel test: with the mark on, pixels in the gutter beside a wrapped row
differ from an unwrapped row and no cell pixel changes.

## Reflow measured against keep-clipped (2026-11-22, agent2; recommendation)

**What was measured.** `bench/reflowbench.cpp` (`matterm-reflow-bench`, Release, one process, this
machine) is a prototype of history reflow on the real storage (`TerminalMemory` rows plus the model's
per-row soft-wrap flags): join rows across `softWrap`, drop trailing default blanks of each logical line
(padding rule: rows have no written extent), lay the line out again, set the flags. It is not wired into
`TerminalModel`: no cursor, saved cursor, viewport anchor, selection or alternate-screen handling. It
checks the round trip (narrow, widen: text and flags restored exactly: yes in all runs). History of 200-column
rows; "short" = every logical line fits one row, "mixed" = 70% short, 25% up to 1.5 rows, 5% up to 5 rows,
"long" = 0.5 to 3.5 rows.

| history rows | width change | mix | narrow | widen back | drag step (one column, whole history re-laid) |
|---|---|---|---|---|---|
| 1000 | 200 -> 100 | short / mixed / long | 4.4 / 1.5 / 2.5 ms | 10.8 / 6.7 / 14.9 ms | 2.8 / 3.3 / 4.1 ms |
| 10000 | 200 -> 100 | short / mixed / long | 44.9 / 17.9 / 15.9 ms | 78.4 / 73.9 / 87.1 ms | 35 / 33 / 30 ms |
| 10000 | 200 -> 66 | short / mixed / long | 23.3 / 16.8 / 10.0 ms | 58.6 / 66.0 / 32.0 ms | 23 / 24 / 21 ms |

Row counts after narrowing 10000 rows to 100 columns: 13 000 to 18 000 (the saved-row limit then trims the
oldest). For comparison, the keep-clipped prototype (table above, same window and history) does a
10000-row narrow+wide pair in 0.2 ms (stored width enough) to 9 ms (window was wider once), a drag step in
0.24 to 0.45 ms; clipping as it is today 10 ms and 5 to 7 ms.

**Steady state (owner's first priority).**
- Reflow needs nothing on the write path: the soft-wrap flags are already maintained by the writer, and the
  padding rule needs no written extent. Steady-state cost = main (0%), memory per row unchanged (width x 12
  bytes), plus the row count growth only while narrow.
- Keep-clipped adds a hook to the mutable `TerminalMemory::at()` (measured earlier on `agent2-keepclipped`,
  13ac6ef: about +6 to 7% on the write-heavy terminal benchmark after the one-counter optimisation, +9 to 12%
  before) and stores up to `keptColumns` (512) cells per row once the window was wide: 12 bytes x rows x
  widest width, 59 MiB per pane at 10000 rows and 512 columns.

**Resize cost.** A single reflow of a full 10000-row history is 15 to 90 ms: inside "milliseconds to a short
pause", but a divider drag re-laying everything at every step (about 30 ms x steps) is not. Keep-clipped
handles a drag in well under a millisecond per step. So reflow needs:
1. coalescing: re-lay only after the width has been stable for ~100 ms (and once more on release), during the
   drag draw the old rows clipped; or
2. lazy reflow: re-lay the live screen and the newest ~1000 rows at once (3 to 4 ms) and the rest in the
   background in slices (or on the first scroll above them), anchored by logical line.
The prototype is a straightforward double loop; a reusable line buffer, no `FChar` default-blank test per
cell and slot-wise copies would likely halve it, but that is not measured.

**Recommendation.**
1. **Go for reflow, skip keep-clipped.** It matches the owner's goal (GNOME Terminal), costs nothing in the
   steady state, and its drag cost is a solvable scheduling problem, while keep-clipped taxes every write and
   still cuts text on screen.
2. **Scope step 1 to the saved history only** (rows above the live screen), as already proposed: the live screen
   and the alternate screen stay clip/pad, programs redraw them after SIGWINCH, the shell-redraw artifacts of
   reflowing the live screen are avoided. Text that scrolls into history after a narrowing is already clipped
   (accepted; it is the same as today for the live screen).
3. **Prerequisites, in this order:** (a) soft-wrap flags survive width changes on the primary screen (small, from
   the keep-clipped prototype, mergeable alone: `13ac6ef`/`ca6e270` terminalmodel part); (b) an audit test that
   every wrapping path sets and clears `softWrap` (autowrap, deferred wrap, ICH/DCH, scroll regions, ED/EL);
   (c) the padding rule written down: trailing default-styled blanks of a logical line are padding, a line the
   user ended with spaces loses them on reflow (cosmetic; wrapped rows are never trimmed).
4. **Then** integrate with the model (cursor mapping is not needed for history-only reflow; the viewport anchor
   and selection/link invalidation are), with the coalescing or lazy scheme above, and re-measure with a Release
   build of the integrated model against `main` (write throughput, scroll of a full history, memory per row).
5. Do not merge `agent2-keepclipped` unless the owner wants the stopgap before reflow.

Limits of these numbers: one machine, synthetic text, the algorithm outside the model, no allocation-failure
rollback measured (the new history is built beside the old, so peak memory is 2x the history for the duration).

**Prepared (2026-11-22):** `RenderFrame::wrapMarks` (one flag per supplied row, empty = unknown) and
`TerminalModel::viewWrapMarks()` (the flags of the current frame's rows, from `softWrap`) exist and
are tested. No backend draws the mark yet: that waits for the reflow work to land, since without
reflow the mark would promise something that does not happen.

## Implemented: history-only reflow (2026-11-22, agent2)

The owner chose reflow of the saved rows (the recommendation above). The live screen and the
alternate screen are clipped and padded as before. Code: `src/terminal/reflow.h` (the layout),
`TerminalModel::Impl::resized` / `reflowSlice` (`terminalmodel.cpp`), `TerminalMemory::prepend`,
`ScrollViewport::rowsPrepended` / `reanchor`. Tests: `test/terminalreflow_test.cpp` (27 cases).
Benchmark: `bench/modelbench.cpp` (`matterm-model-bench`).

### What a width change does

* The saved rows are joined into logical lines along the soft-wrap flags that the model already
  keeps (set by autowrap when the deferred wrap fires, cleared when the row is erased to its end,
  scrolled out of a region, or replaced) and cut again at the new width. A new row carries the flag
  when the line goes on in the next row. Cell styles move with the cells.
* **Boundary-line rule.** The last row of a logical line never carries the flag, even when the
  input row did: the flag of the newest saved row means "goes on in the live screen" and the live
  rows are not reflowed, so the line ends there (it is cut, as on a narrow clip).
* **Growth** reveals nothing new in the live rows: they are padded with blank cells; the history
  that had been cut is not restored, the lines are simply laid out again.
* **Operations that blank the end of a row** (DCH, EL/ECH reaching the right edge) clear the flag:
  the line ends there. This is conservative: at most a line breaks at a different place.
* Eviction: the ring keeps its capacity in physical rows. If the new layout has more rows than fit,
  the oldest go. A line is cut at its start, never in the middle.

### Padding rule

At the end of a logical line, trailing cells that look never written are padding and are dropped,
so a line that filled 80 columns with 20 characters does not become 4 rows of blanks at 20 columns.
A cell is padding when it is a space or NUL, has no Underline/Strike/Inverse, and its background is
the erase background at that moment or the pristine default (`Screen::defaultCell()`). A typed
space in a coloured background, or an underlined space, is text and stays. Blanks inside a line
always stay. A bounded run (`kMaxPendingPadding`, 8192 cells) is held back while deciding.

### Lazy reflow and drag

Reflowing every saved row on every step of a drag is what cost 30 ms per step at 10000 rows. So:

* The newest 1000 saved rows (`kImmediateRows`) are laid out in the `resize()` call, always cut at
  a logical-line start. The older rows stay as a **segment**: the old Screen is kept untouched, at
  the old width (at most `kMaxSegments`, merged when it is cheaper to drop; segments under
  `kMinSegmentRows` are laid out at once).
* A new resize while a segment waits does not touch the segment: it is cut again from the
  rows already laid out, the segment keeps its old width.
* `advanceViewport(dt)` (called every frame by all frontends) counts quiet time since the last
  resize; after `kQuietSeconds` (0.1 s) it lays out one slice of `kSliceRows` (1000) older rows and
  **prepends** them above the oldest row. Row ids of existing rows do not change (new rows get
  smaller ids; `firstRetainedRow()` decreases), the selection epoch is not bumped by a slice.
* The reader never sees a gap: rows the reader's view covers are laid out at once, and
  `ScrollViewport::reanchor` keeps the first visible row on the same logical position (a reader at
  the tail stays at the tail). Positions are measured from the live bottom, so prepending older
  rows does not move the view.
* `reflowPending()` says if rows wait; `finishReflow()` does all of it now (tests, shutdown).
  `eraseScrollback()`, a full ring (nothing can be prepended) or `bad_alloc` drop the segments.
* Cost: while a segment waits the old rows exist twice (old width and the laid-out part), at most
  the ring capacity again. Writing while a segment waits works as usual.

### Measured (Release, `matterm-model-bench`, 1 column per step; main 78b5a9e = "base")

Steady state (24000 lines of ~75 characters through a 10000-row history, best of 5):

| | base | reflow |
|---|---|---|
| write, 80 columns | 29.6 / 30.6 ms | 28.7 / 29.0 ms |
| write, 200 columns | 31.9 / 32.6 ms | 31.1 / 31.2 ms |

No difference beyond noise: the write path, scrolling and memory per row are unchanged (the flags
were already kept; nothing was added per row).

Drag (history of N lines, capacity 20000 rows; mean per `resize()`+`advanceViewport` step):

| lines | narrow 200→80 base / reflow | widen 80→200 base / reflow | settle (rest of the rows) |
|---|---|---|---|
| 1000 | 11.8 / 12.7 ms | 4.0 / 4.7 ms | none |
| 5000 | 10.0 / 13.0 ms | 5.4 / 4.4 ms | 14 ms narrow, 20 ms widen |
| 10000 | 6.2 / 8.8 ms | 7.4 / 4.4 ms | 15 ms narrow, 43 ms widen |

The cost per step is the allocation of the new ring (as before) plus about 1000 rows of layout;
the rest (settle) is spread over frames after the drag, 1000 rows per frame.

## Implemented: reflow of the live rows of the primary screen (2026-11-22, agent2)

Owner: the visible screen should be reflowed like GNOME Terminal, not clipped. Alternate screen
unchanged (clipped, no history). `TerminalModel::setLiveReflow(Off | AboveCursor | All)`, default
`AboveCursor`. Code: `Impl::resizedLive` in `terminalmodel.cpp`, `reflow::Anchor` in `reflow.h`.

### Model

The saved rows and the screen rows are one stream of lines; a width change lays all of it out again
and the last `height` rows (counting from the end of the content) are the new screen. Rows below the
cursor that are blank are not content: they are added again after the last row. So narrowing sends
rows to the history, widening pulls them back, a screen that is only partly used keeps its content at
the top. If more non-blank rows follow the cursor than fit, the cursor stays on the screen and the
rows after it are dropped (rare: full-screen programs use the alternate screen).

Followed through the layout by their place in their logical line (`reflow::Anchor`, cell index in the
line, so the same code serves the lazy path): the cursor, the saved cursor (DECSC) and the first row
the reader scrolled to. A cursor behind the text of its line (a prompt ending in a blank, typed text
followed by padding) keeps the padding in front of it; a cursor far out in an empty row is pulled
back into the last row of its line (no rows are made for it); a cursor directly behind a full row is
the pending-wrap position (`wrapPending`) of that row.

### The shell problem, measured with real bash

Shells redraw their prompt when the size changes (SIGWINCH) and assume the terminal did not reflow:
bash/readline goes to column 0, clears and writes prompt and input again, moving up as many rows as
the prompt had at the *old* width. I recorded bash 5 on a pty (prompt 30 and 50 characters, typed
input, 80→20→80 and 40→80→25 columns) and played the bytes into the model:

* `All` (VTE-like, the cursor's line is reflowed too): after narrowing the prompt line is already two
  or three rows; bash writes at the cursor row, below those rows: **a copy of the prompt stays above
  the new one** (two prompts after 80→20, four rows). Widening merges the rows and bash moves up by
  its old row count into earlier output.
* `AboveCursor`: the logical line the cursor is in and the rows below it are clipped and padded as
  before (their flags are kept); only what is above it is reflowed. bash redraws over its own rows
  and the result is exactly one prompt, at 80→20→80 and 40→80→25. This is the default. The test
  "a shell that redraws its prompt after the resize leaves no copy behind" does the same with
  CR/EL/ED and asserts the copy for `All` by design, so a change of default shows up.
* `Off`: only the saved rows are laid out again (the earlier implementation).

Cost of `AboveCursor`: a long line being edited or a program that printed a long line without a
newline and waits stays clipped on narrowing until it redraws. A terminal that wants VTE behaviour
sets `All` and accepts the copy (or recognises prompts, which needs shell integration, not done).

### Cost

Steady state is unchanged: nothing was added to the write path or to scrolling (modelbench, Release:
80 columns 30.1 ms against 31.0 ms on main, 200 columns 35.8 against 38.6, i.e. noise). A drag step
lays out the newest 1000 saved rows plus the screen rows (lazy rest as before): narrow 200→80 with
1000/5000/10000 lines 14.0/13.9/10.3 ms against 13.3/11.6/7.4 ms on main, widen 5.3/5.1/5.4 ms
against 4.7/6.4/8.5 ms (the cost is dominated by allocating the new ring in both). The rest of a
10000-line history settles in 19 ms (narrow) and 48 ms (widen) in total, 1000 rows per frame.

### Tests

`test/terminalreflow_test.cpp` (40 cases): cursor placement (mid-line, behind a prompt, far out in an
empty row, pending wrap), saved cursor, blank rows below the cursor, rows to and from the history,
reader anchoring, content longer than the screen below the cursor, random output/width/height mixes
in both modes, "all text survives any width in order" in both modes, the bash-like redraw, flags of
the clipped cursor line. `terminalmodel_test` states the mode it relies on.
