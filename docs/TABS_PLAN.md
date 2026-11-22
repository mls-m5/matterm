# Tabs, panes and windows: architecture and implementation plan

Status (2026-11-21): phases 0, 1 (A model, B rendering, C strip) and 2 (native app) are
built and on `main`; the browser (phase 3) is in progress; phase 4 is open. What exists:
`src/frontend/panetree.h`, `workspace.h`, `prefixkeys.h`, `tabstrip.h`,
`workspaceview.h` (the SDL-free glue: frame plan and input routing), the native app
in `app/ptyfrontend.cpp`, `SdlBackend::beginFrame/render(frame, PaneArea)/drawRect`.
Tests: `panetree`, `workspace`, `prefixkeys`, `workspace_view`, `tabs_stress` (real
shells: close sends SIGHUP and kills after 100 ms, no zombies or descriptors left,
hidden panes served, resize storm throttled) and `pty_frontend_tabs_smoke` (real SDL
events in the dummy driver: splits, a hidden tab that keeps running, a divider
drag). Run under address and undefined sanitizers without findings. Not verified on a
real screen or window manager. Native window snapping is gone (pixel splits make it
unnecessary). Decisions of the owner are in
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

## 7. Lifecycle review of the native app (2026-11-21)

Checked with `test/tabs_stress_test.cpp` (real shells) and the sanitizer build:

- **Closing a pane or tab** destroys its `PtyProcess`: the PTY master is closed, SIGHUP
  goes to the process group and the child, and after 100 ms without exit both get
  SIGKILL and are reaped. A normal shell ends in a few ms; a program that ignores
  SIGHUP costs the GUI thread 100 ms (an 8-pane tab of such programs about 0.8 s,
  because the wait is blocking). A background job of the shell is in the same group
  and is hung up too. No zombies remain (`waitpid(-1)` says no children).
- **A program that exits** (also in a hidden tab) is noticed in the pump loop; its pane
  closes, the tab with its last pane, the window with its last tab. The exit notice
  the view writes is never seen, since the pane is gone at once; a "keep the pane open
  after exit" option would need a view state in the strip. The window's exit code is
  that of the last pane that ended.
- **Descriptors:** 40 rounds of new tab, three splits and close (alternately whole tab
  and pane by pane) end with the same number of open descriptors as they started.
- **Resize storm:** 3000 divider events at 1 ms spacing give at most one size change
  per 40 ms, and at the end every program reports (`stty size`) the planned size.
  Throttling is by `ViewConfig::resizeIntervalMs`.
- **Open:** new panes are forked after SDL has started threads (the first one before);
  the child only closes descriptors and execs, but a reaper thread or a helper process
  started early would be safer. Hidden panes are parsed on the GUI thread (up to
  64 KiB per pane per frame), so many busy panes cost frame time.

## 8. Discoverability, overlays, settings and a small cell UI toolkit (proposal, 2026-11-22)

The owner finds the key bindings hard to remember. Nothing here is decided or built.

- **One command registry.** `Command` plus metadata (name, short description, default
  shortcut per platform) as the single source for the key tables, the palette, the
  help view and the docs. Rebinding later is a setting on top of it.
- **Ways to find a command**, cheapest first:
  1. *Which-key hint:* after the prefix (browser) a small box appears after about half
     a second and lists the next keys (`c` new tab, `%` split ...). Nothing to open.
  2. *Help view:* prefix `?` (desktop: a shortcut) lists all commands and keys, scrollable.
  3. *Command palette:* a text field with a filtered list of commands and their keys;
     Enter runs the command. Desktop Ctrl+Shift+P; browser prefix `:`. Also the way
     to open a tab or split without remembering anything.
  4. *Mouse menu:* a button at the end of the tab strip (and right-click) with the
     same list, shortcuts shown beside each entry.
- **Overlays need a layer stack, not special rendering.** An overlay is a cell grid drawn
  by one more `render(frame, PaneArea)` after the panes (the backend already draws a
  frame into a rectangle), with its own border, over a cleared area. Input goes to the
  topmost layer first: overlay, then strip/dividers/scrollbars, then the pane. The
  layer stack lives in `WorkspaceView`; the frame signature must include the overlay.
- **A small cell UI toolkit follows naturally** (SDL-free, in `src/frontend/ui/`, tested
  without a window): label, text input, list with a filter, checkbox, choice, button,
  vertical/horizontal layout, a focus ring, key and mouse events in, actions out, a
  `CellBuffer` out. Start with only what the palette and the help view need (text
  input, list, scrollable text), and add widgets when the settings view needs them.
  It can be reused by Mattrix OS programs and the chat screen later. This is not a
  general TUI framework: no terminal output backend, no theming engine at first.
- **Settings view** needs a settings model first: what can be set (font size, prefix
  key, smooth cursor, sound, palette, scrollback), where it is stored (desktop: a
  config file; browser: localStorage) and how it is validated. The view is then a
  list of widgets over that model. Do the model and the palette before the view.
- **Suggested order:** command registry, which-key hint, layer stack, palette and help
  view (this forces text input, list and scroll), mouse menu, settings model, settings view.

Built so far (2026-11-22, browser): `WorkspaceView` has a layer stack (`pushLayer`,
`ui::Layer` in `src/frontend/ui/layer.h`; the top layer takes keys, clicks and the wheel,
a click outside closes it; `FramePlan::layers` carries each overlay as a `CellBuffer`
and a pixel rectangle, which the owner draws after the panes and includes in the frame
signature). `ui::TextInput`, `ui::ListView` and `ui::CommandPalette` (filtered by
`searchCommands`, keys from the registry) are SDL-free and tested (`test/ui_test.cpp`).
`Command::OpenPalette` (prefix `:`, desktop Ctrl+Shift+P) and the strip's button (glyph
U+2261, `ViewConfig::paletteButton`) make `KeyResult/MouseResult::openPalette` true; the
owner creates the palette and pushes it. Wired in `web/main.cpp` only; the native app
still ignores `openPalette`. The button is part of the strip, so it shows with two or
more tabs; with one tab the prefix `:` is the way in. U+2261 is not yet checked
against the bundled font (needs `CHAT_TEST_FONT`).

Open items noted by the owner:
- The scrollbar of the left pane (in the divider gutter) is hard to see: in the browser
  run of 2026-11-22 it is a thin sliver (about 3 px) next to the divider and takes
  the divider's grey, while the pane at the window edge has the full 12 px bar.
  (Browser: the gutter is now 16 px in a dark colour, so the bar half is 8 px and the
  thumb stands out; to be checked on a screenshot.)

### Naming a tab (owner request, 2026-11-22)

- A tab has an optional **user title**. Shown title = user title, else the title the
  program set (OSC 0/2, already sanitized by the strip), else "Tab N". An empty input
  clears the user title and returns to the automatic one.
- Ways to set it: double-click on the tab (an inline text input in the strip, Enter
  accepts, Esc cancels); prefix `,` in the browser (as in tmux); a "Rename tab" entry
  in the command palette and the mouse menu; a desktop shortcut (F2 on the tab).
- Stored in the layout snapshot (`LayoutSnapshot`), so it survives a reload. It is
  untrusted text on restore: same limits and sanitizing as program titles, length cap.
- Needs: a `RenameTab` command with a text argument (the palette's text input is the
  first widget that produces it), a title field in `Workspace`'s tab, the snapshot
  format change (bump its version, keep reading the old one).

## 9. Saving and restoring sessions (idea, owner request 2026-11-22, nothing decided)

Goal: after the terminal is restarted, keep the layout, the folders and, if it can
be made to work, the programs that were running.

What can be restored, from cheap to hard:

1. **Layout:** tabs, the pane tree, ratios and tab titles. `LayoutSnapshot` (strict,
   limits, versioned) already does this for the browser; native can use the same format.
2. **Working folder of every pane.** On Linux read `/proc/<pid>/cwd` of the pane's
   foreground process (via `tcgetpgrp` on the PTY, falling back to the shell). It needs no
   shell configuration, which the owner prefers (OSC 7 would need the shell to print the
   folder; use it too when a shell does, but do not depend on it). Linux only; elsewhere
   the layout comes back without folders.
3. **What was on screen.** The checkpointed on-disk scrollback idea
   (`docs/MULTI_TERMINAL.md` section 12): a restored pane can show its old text,
   dimmed, under a marker line, then the new shell starts below it in the same folder.
4. **The command line of the program that was running** (`/proc/<pid>/cmdline`). A
   running process cannot be moved (no checkpoint/restore of processes), so the
   program can only be started again. Never automatically: the pane shows "was
   running: <command> - press Enter to run it again" and waits. Environment variables
   are not saved (they may hold secrets). Shell state (variables, an activated virtualenv,
   an open ssh session, unsaved history in memory) is lost.
5. **Processes that really keep running** only if they are owned by something that
   outlives the window: the server's persistent sessions already do this for the
   browser. The native app could attach to a local session daemon over a Unix socket
   (the tmux model: close the window, programs go on, open it again and attach). That
   is the only way to keep a program alive across a restart of the terminal, and it needs
   a local transport for the server, an owner-only socket, and a rule for who may attach.
   After a reboot of the machine all processes are gone, so 1-4 are the fallback.

Policy ideas:
- The default session is saved after every layout change (debounced) and on quit, and
  offered or restored at the next start. Named sessions ("work", "server") can be saved
  and opened from the command palette.
- Stored under the XDG state folder, one file per session, mode 0600, versioned, read with the
  same strict parser and limits as the browser layout (the file is untrusted input).
- A restored pane whose folder no longer exists starts in the home folder with a note.
- Window position and size are saved but only used where the window manager lets us
  (not on Wayland).

Suggested order: (1) layout + titles + folders in the native app, (2) restored text
under a marker, (3) "run it again" offer for the previous command, (4) named sessions in
the palette, (5) local session daemon, if the owner wants the tmux model.

Questions for the owner: restore automatically or ask at start? Named sessions
wanted? Is "offer to run the program again" enough, or is a daemon that keeps programs
alive the goal?
