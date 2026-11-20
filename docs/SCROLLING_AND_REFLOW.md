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
