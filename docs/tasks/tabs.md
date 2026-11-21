# Tabs and panes: workstreams

Read `docs/TABS_PLAN.md` first (architecture, phases) and `docs/MULTI_TERMINAL.md`
section 13 (the owner's decisions). The shared vocabulary is
`src/frontend/tabtypes.h`; change it only after telling the integrator.
Base: branch `main` including the `tabs-base` step (TerminalView, cellhit, viewport in
`SdlBackend`). No pushes; commit on your own branch and report the SHA.

Defaults chosen by the integrator while the owner is away (change only with a reason):
prefix key Ctrl+B, then: `c` new tab, `n`/`p` next/previous tab, `0`-`9` select tab,
`%` split side by side, `"` split stacked, `x` close pane, arrows or `h j k l` move
focus, `=` even out, `Ctrl+B Ctrl+B` sends a literal Ctrl+B; the prefix times out
after 2 s. At most 8 panes per tab. A pane is at least 20 columns by 5 rows.

## A. Pane tree and workspace (agent2) -- `src/frontend/panetree.h`, `workspace.h`, `prefixkeys.h`

Pure C++17, header-only like the rest of `src/frontend/`, native tests only.
- `PaneTree`: leaf or split with a ratio (fraction), split a leaf, close a leaf (the
  sibling takes the space), at most 8 panes, minimum pane size in cells, focus by id
  and by direction (by geometry, not tree order).
- `layout(PixelRect area, int cellWidth, int cellHeight, int dividerThickness)` returns
  `std::vector<PaneBox>` and `std::vector<DividerBox>` (see `tabtypes.h`). Whole cells
  per pane, the leftover as `leftover[]`, no pane smaller than the minimum, rounding
  never loses or gains pixels (the boxes plus gutters tile the area exactly).
- Divider drag: `setRatio` from a pixel position, clamped by the minimums; with
  `snapToCells` it snaps to whole cells; `evenOut()`.
- `Workspace`: `unique_ptr<Pane>` ownership as in `TABS_PLAN.md` section 2 (the pane
  type may be a template parameter or hold a `TerminalView` -- tests use a fake), tabs,
  active tab, activity mark per hidden pane and tab, the parked list, `apply(Command)`.
- `PrefixKeys`: a state machine from key events to `Command` (or "pass through to the
  program"), with the timeout and the literal-prefix rule above. Also the desktop
  shortcut table (Ctrl+Shift+T/W, Ctrl+PgUp/PgDn, Alt+1..9) as data.
- Tests: tiling exactness for many sizes and trees, minimums, close/split sequences
  (property style with a seed), focus by direction on an L-shaped layout, ownership
  (a pane moved to the parked list is neither lost nor destroyed twice).

## B. Rendering (agent3) -- `src/render/` -- DONE (see the API note below)

- A frame is `beginFrame` (clear once), any number of `render(frame, viewport)`, then
  dividers, then `present()`. Today `render()` clears everything; keep the old single
  call working. A "frame" for caches is a `present()`.
- Draw dividers (`DividerBox`) as filled gutter strips and a pane's scrollbar in the
  half of the gutter next to it (pane at the window edge: the reserved strip as today),
  taking no columns from the program.
- Paint a pane's `leftover` pixels like the cell beside them (as the reserved
  scrollbar strip already is).
- `FrameSignature` must cover every pane, divider and the strip so an idle window
  still costs nothing.
- Atlas: add the generation number that grows at each `present()` (no eviction yet;
  `TABS_PLAN.md` section 4 says what comes later).
- Tests with the dummy driver where possible (`sdlbackend_test`).

## C. Tab strip in cells (agent3 after B, or agent2 after A) -- `src/frontend/tabstrip.h` -- DONE

- `buildTabStrip(const std::vector<TabInfo> &, int columns)` returns one row (later two)
  of cells (use the cell types of `src/core`) plus hit regions: tab i, close button of
  tab i, the new-tab button, and the overflow rule (shrink titles, then show numbers,
  then scroll). Titles are plain text and are cut by display width, never mid
  character; wide characters count two cells.
- `hit(column)` returns what is under a column. Style like the chat screen
  (`src/chat/chatview`), colors from the palette, the active tab and the activity mark
  clearly visible.
- Tests: widths from 1 to 200 columns, wide characters, 0 and 1 and 50 tabs.

### API as built

- B: `SdlBackend::beginFrame(Rgb8)`, `render(frame, PaneArea{rect, scrollbar})`,
  `drawRect(PixelBox, Rgb8)` for dividers/chrome, `atlasGeneration()` (grows at each
  `present()`, kept across `setPixelSize`). `PixelBox`/`PaneArea` are in
  `sdlbackend.h` (render does not include `tabtypes.h`; convert `PixelRect` at the
  call site). The old `render(frame)` is unchanged. Inside `render(frame, area)` the
  pane's leftover pixels (right of and below the last whole cell) are painted like
  the edge cell; a non-empty `scrollbar` rectangle (the half of the gutter) gets the
  track and thumb and takes no columns. `frontend::windowSignature(panes, dividers,
  w, h)` (in `framesignature.h`) hashes every pane with its place, every divider
  and the strip (pass the strip as a pane entry).
- C: `buildTabStrip(tabs, columns, palette, firstVisible)` returns a `TabStrip`
  (cells, disjoint `StripRegion`s, `hit(column)`, mode, firstVisible/visibleCount).
  Numbers are 1-based. Hit kinds: Tab, Close, NewTab, ScrollLeft, ScrollRight.

## Later (integrator): native app, then the browser

See `TABS_PLAN.md` section 5, phases 2 and 3.
