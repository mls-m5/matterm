# Tabs, panes and windows: architecture and implementation plan

Status: plan only, nothing of this is built on `main`. Decisions of the owner are in
`docs/MULTI_TERMINAL.md` section 13 (chrome in cells, tree of panes, a tab is a whole
layout, pixel splits, prefix keys in the browser, GNOME Terminal keys in the desktop
app). Written 2026-11-21 after a discussion with the owner.

## 1. What exists

On `main`: `TerminalModel` and the parser, `SdlBackend` (one renderer, one
`GlyphAtlas`), `runPtyFrontend` (one window, one `PtyLink`), `ClientSession` and the
server with sessions (`SessionRegistry`, replay ring, take-over), the selection and
scrolling code.

On `archive/web-tabs` (finished, browser only, not on `main`), and reusable:

| File | What | Fate |
|---|---|---|
| `src/frontend/terminalview.h` | One terminal: model, parser, palette, title, selection, cursor glide, fed by one `TerminalLink` | Reuse as is |
| `src/frontend/cellhit.h` | Pointer position to cell for a pane of any size | Reuse |
| `src/render/sdlbackend.*` | `setViewport`, drawing into a pane | Reuse, then extend (section 4) |
| `src/frontend/tabmodel.h` | Tabs and at most two panes | Replace by the pane tree (section 3) |
| `web/tabs.js`, `web/tabpolicy.js` | HTML tab strip | Dropped (chrome is drawn in cells) |
| `web/main.cpp` (list of views) | Browser integration | Rework on the new model |

## 2. Ownership (one direction, no cycles)

```
Application
 +-- Workspace
 |    +-- Tab[]            each Tab owns one PaneTree
 |    |    +-- PaneTree    nodes: Split{direction, ratio, children} | Leaf
 |    |         +-- Leaf   owns unique_ptr<Pane>
 |    +-- parked           std::vector<unique_ptr<Pane>>, not drawn, still running
 +-- Window[]              SDL_Window + SdlBackend (renderer + atlas) + a Workspace view
Pane = unique_ptr<TerminalView> (+ its link: PtyLink native, ClientSession browser)
```

- A pane owns its process, and a process belongs to at most one pane (`unique_ptr`).
  Moving a pane between a tab, the parked list and another window is `std::move`.
- Focus and the like use ids or non-owning pointers, never ownership.
- A `TerminalView` knows nothing about windows, atlas or SDL; it can produce a
  `RenderFrame` when asked. The window draws whole frames into viewports.
- **One backend per window, never one per pane.** Panes share the window's atlas
  without holding any pointer to it. The atlas texture belongs to its renderer, so
  a separate atlas per window is forced by SDL anyway (and windows on screens with
  different DPI need their own font size).
- Several windows in one process: one event loop, events dispatched by `windowID`,
  all on the GUI thread. Moving a pane to another window is the `std::move` above.
  Between separate processes the server is the way: attach the same session.
- Sessions of the browser stay server-owned (`docs/MULTI_TERMINAL.md` section 3); a
  tab stores the session ids of its panes, and the layout can be restored from them.

## 3. The model (pure C++, no SDL, tested natively)

- `PaneTree`: split a leaf (horizontal or vertical), close a leaf (the sibling takes
  the space), ratios as fractions, minimum size per pane (in cells), focus by id and
  by direction (left/right/up/down by geometry), no more than a fixed number of panes.
- `layout(area in pixels, cellWidth, cellHeight)` returns for every leaf its pixel
  rectangle, how many whole cells fit (columns, rows) and the leftover pixels, and
  for every divider its rectangle (the gutter strip), orientation and the two panes
  it separates. A divider drag changes a ratio; with Shift it snaps to whole cells;
  "even out" snaps every split.
- `Workspace`: tabs, active tab, activity mark per tab (set when a hidden pane gets
  output or rings), the parked list, new tab, close tab, select, move.
- Commands as data (`NewTab`, `Split(dir)`, `ClosePane`, `FocusLeft`...), so that key
  bindings, the prefix state machine and the mouse all produce the same commands and
  tests need no window. The prefix machine: first key (default Ctrl+B, configurable),
  then a letter within a timeout, otherwise the keys go on to the program.
- Desktop shortcut table (GNOME Terminal style): Ctrl+Shift+T, Ctrl+Shift+W,
  Ctrl+PgUp/PgDn, Alt+1..9, Ctrl+Shift+C/V, plus own keys for splitting and moving.

## 4. Rendering

- **Several renders in one frame.** `render()` currently clears the whole renderer
  (`sdlbackend.cpp`, `SDL_RenderClear`); the archive branch clears only the pane.
  A frame should be: begin (clear once), `render(view, viewport)` any number of
  times, draw the dividers and the tab strip, `present()`. A "frame" for caches
  means a `present()`, not a `render()` call.
- **Tab strip in cells.** A row (or two) of cells built from the workspace state
  (titles as plain short text, the active tab, the activity mark, a close button,
  a new-tab button), with hit regions in cells. Drawn by the same cell renderer, so
  it looks the same in the browser and in the native app.
- **Dividers and scrollbars in pixels.** A divider is a gutter strip at a pixel
  position; a pane's scrollbar is drawn in the half of the strip that is next to it,
  so it takes no columns from the program. A pane at the window edge keeps a
  reserved strip (12 px today). The leftover pixels of a pane (not a whole cell) are
  painted like the cell beside them, as the reserved strip already is.
- **Frame signature** must cover every pane, the dividers and the strip, so an idle
  window still costs nothing.
- **Glyph atlas.** Today the atlas has a fixed size, never evicts and throws "Glyph
  atlas is full" (`glyphatlas.cpp`). Notes for later, to be done when it becomes a
  problem (more panes and more kinds of text make it likelier):
  - A `Glyph` is a copied rectangle, valid only for the frame that looked it up; it
    must not be kept longer. `render()` collects its glyphs first and then draws.
  - Never reset or move glyphs in the middle of a frame. Give the atlas a
    generation number that grows at each `present()`, and let a slot be overwritten
    only if it was not used in the current generation. The font is monospace, so
    slots of cell size (and larger ones for wide characters and emoji) are enough.
  - If the atlas is full of glyphs used in this frame: start a second atlas texture
    ("page") for the rest of this frame and draw in two passes (`Glyph` then needs
    a page number). Growth or a full reset happens only at the start of the next
    frame, with the new size or page set; the glyphs are rasterized again when next
    asked for. The GPU has a largest texture size (`SDL_GetRendererInfo`).
  - Windows with the same font and size could later share the rasterized bitmaps on
    the CPU and upload them to their own textures. Not needed at first.
  - Not verified: whether SDL flushes queued geometry before `SDL_UpdateTexture` in
    our SDL version; the generation rule above avoids depending on it.

## 5. Phases and what can run in parallel

**Step 0, first and short (integrator):** cherry-pick `TerminalView`, `cellhit` and the
viewport support of `SdlBackend` from `archive/web-tabs` onto a branch off `main`,
keep their tests, build warning-free. It does not change behavior. Everything below
starts from it. Also agree the interfaces in section 3 (`Rect`, `PaneTree::layout`,
the command list) in a header with tests, because the workstreams meet there.

**Phase 1: the model and the drawing, three independent workstreams**

- **A. Pane tree and workspace** (pure logic, `src/frontend/`, native tests). Section
  3: tree, layout in pixels with leftover, divider geometry, snap, focus by direction,
  workspace and tabs, parked list, commands, prefix state machine, shortcut table.
  No SDL, no window. Fit: the agent doing model and review work.
- **B. Rendering** (`src/render/`). Section 4: several renders per frame with a viewport
  clear, drawing divider rectangles and the pane scrollbars in the gutter, leftover
  pixels painted like the neighbor cell, the frame signature for several panes, the
  generation counter in the atlas (eviction itself can wait). Visual tests with the
  dummy driver where possible. Fit: the agent that knows the cell renderer.
- **C. Tab strip in cells** (`src/frontend/`, uses the cell types only). Build the
  strip as a cell grid from workspace state, with hit regions, in the style of the
  existing chat screen. It needs only the data shape of A, so it starts on the
  interface header of step 0.

**Phase 2: native app** (`app/ptyfrontend.cpp`, integrator or one agent after A and B).
Replace the single session with a `Workspace`: a `PtyLink` per pane, pane resize on
the cells that fit, the mouse routed first to scrollbars, dividers and the strip and
then to the pane, selection per pane, the desktop shortcuts, the window snap per
whole window. Acceptance: two tabs, a tree of three panes, a background process
that keeps running in a hidden tab, resize by dragging a divider.

**Phase 3: browser** (`web/main.cpp`). The same on the browser side: a
`ClientSession` per pane, the prefix keys plus the mouse, the layout and session ids
restored after a reload (sessionStorage), the server flow of `docs/tasks/web-sessions.md`.
Can start when phase 1 is merged, in parallel with phase 2, because the model is
shared; the e2e checks of `archive/web-tabs` are the starting point for new ones.

**Phase 4:** scrollbar polish (fade, drag), activity marks refined, move a pane to
another tab or window, the atlas generations and pages if measurements ask for them.

Later and outside this plan: several windows in one process, width reflow,
recording and playback, shell integration.

## 6. Risks and open points

- Pane resize sends SIGWINCH to programs at every whole cell crossed during a
  drag; throttle if programs cannot keep up.
- Wide screens with several tabs: the strip needs a rule for overflow (scroll or
  shrink to numbers).
- Mouse reporting: when a program has mouse tracking on, the strip, dividers and
  scrollbars must still take their own events first (Shift passes the event on
  to the pane, as today for selection).
- The browser keeps Ctrl+T/N/W; only the prefix keys and the mouse are available there.
- Native window resize: snap to whole cells only after the size was quiet (see
  `ONGOING.md`); with pixel splits the window need not snap at all.
- Open: the prefix letters, the divider look, the limit on panes per tab.
