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

### Decided and built (2026-11-22, branch `agent2-savedsessions`, model and files only)

Owner decisions: **no automatic restore** (an opt-in setting later); a session is chosen from
a list (palette style); a previous command is only **offered** ("was running: X - Enter to run
it again"), never started; a session is saved when the owner **names a tab** and on a
controlled shutdown (quit, SIGTERM, SIGHUP) of a window whose session is saved, then with a
text checkpoint of the visible screen. Closing a window or tab that runs a program other than
the shell asks "Close / Cancel"; a third answer "run in background" is future work.

Pieces (all tested, none wired into the app yet, because the dialog and the list need the
overlay layer and the palette from agent3):

| Piece | File | Notes |
|---|---|---|
| `SessionSnapshot` | `src/frontend/sessionsnapshot.h` | JSON v1, strict reader (exact schema, duplicate keys, depth, size and value limits, UTF-8, no surrogates), same tree and pane limits as `LayoutSnapshot`; folder, command, screen text and titles are cleaned or refused |
| `probePane`, `commandLine` | `src/frontend/paneprobe.h` | foreground process folder and command line through a `ProcSource`; shells recognized by name; hostile data (newline in an argument, relative folder) not kept; shell quoting of the offer |
| `LinuxProcSource` | `src/platform/posix/procinfo.*` | /proc/<pid>/cwd and cmdline, bounded |
| `PtyProcess::childPid`, `foregroundProcessGroup` | `src/platform/posix/ptyprocess.*` | `tcgetpgrp` on the master; tested with a real bash (cd, a running `sleep`, Ctrl+C) |
| `SessionFiles` | `src/platform/posix/sessionfiles.*` | `~/.config/matterm/sessions/<name>.json` (XDG_CONFIG_HOME first), folder 0700, files 0600, temp file + fsync + rename, loads only files we own, not group/world writable, no symlinks, name inside must match the file name |
| `captureSession`, `restoreSession`, `visibleText`, `offerText`, `rerunInput`, `SessionPolicy` | `src/frontend/sessionrestore.h` | built on `Workspace::foldTabs` and the commands, the way the browser restores; `willCreate(SavedPane)` tells the frontend the folder of the next shell |
| `Workspace::setUserTitle` | `src/frontend/workspace.h` | the owner's tab name wins in the strip (the rename UI is section 8) |
| `CloseRequest`, `WorkspaceView::setBusyProbe/windowClose/answerClose` | `src/frontend/closeprompt.h`, `workspaceview.h` | keys, the strip's x and middle click ask before closing a busy pane or tab; the answer closes by tab id; `backgroundAvailable` is the hook for the third answer |

Wired in the native app (2026-11-22, branch `agent2-nativewire`): Ctrl+Shift+P and the strip button
open the palette (extra entries *Open session...*, *Save session as...*, *Rename tab...*, built from
`ui::PickList` and `ui::TextPrompt` in `src/frontend/ui/dialogs.h`); the close question is a
`ui::ConfirmDialog` (Cancel is the default button, Escape cancels, plain letters do nothing) for
keys, the strip's x, the window's close button and Ctrl+Shift+Q; SIGTERM and SIGHUP leave the loop and
save a session the owner made persistent, with the screen text; a restored pane starts its shell in
the saved folder (`cd`, then `exec` of the usual command; a vanished folder is noted) and gets the
offer typed on its prompt without a newline. `--pty-tabs-smoke-test` walks through the palette, a
save and open, and both close questions. What was still to do: set the probe (`probePane` over `PtyLink::childPid/foregroundProcessGroup`), call
`windowClose()` on SDL_QUIT and show the dialog from `CloseRequest::question()`, a signal
handler for SIGHUP/SIGTERM that quits through the same path, the session list, the rename
action calling `SessionPolicy::shouldSave(TabNamed)`, and typing `rerunInput()` into a restored
pane's shell. Tokens, environment variables and shell state are never saved.

Questions for the owner: restore automatically or ask at start? Named sessions
wanted? Is "offer to run the program again" enough, or is a daemon that keeps programs
alive the goal?

## 10. A local daemon so the GUI can be restarted without losing processes (proposal, owner 2026-11-22)

Why: while the GUI is developed it must be possible to restart it (new build, crash)
without killing the shells and programs in it. It also is the "programs keep running"
option of section 9. Constraint from the owner: the daemon must change rarely, so that
GUI changes never force a restart of the daemon (and with it the loss of every process).

What exists: `matterm-server` already keeps sessions beyond a connection (persistent
sessions, replay ring with absolute offsets, take-over, `docs/PROTOCOL.md` with a version in
`Hello` and a feature list in `Welcome`). It does not parse terminal output (docs/MULTI_TERMINAL.md
section 4), which is exactly what keeps a daemon small and stable: it owns PTYs, child
processes and raw byte rings, nothing about cells, fonts, tabs or panes.

Design:
- **Local transport:** a Unix domain socket in `$XDG_RUNTIME_DIR/matterm/` (directory 0700),
  peer user id checked (`SO_PEERCRED`), no network, no password. Messages are the
  existing protocol with length-prefixed framing instead of WebSocket frames.
- **One client path:** the native pane becomes `TerminalView` + `ClientSession` over that
  socket, the same code as the browser. The in-process `PtyLink` stays as an option
  (ephemeral mode, no daemon) for tests and for people who do not want a daemon.
- **Lifecycle:** the GUI starts the daemon when it is not running (like `ssh-agent`/tmux),
  and the daemon exits by itself when it has no sessions and no client for a while. A
  session ends when its process ends or the user closes the pane, never because a GUI left.
- **Palette entries:** "Stop daemon" (asks first, naming how many programs would end),
  "Quit and keep running" (detach everything), "Show daemon status", and later "Restart
  daemon after an update".
- **Stability rules for the daemon:** the protocol version only grows by additions within
  a major version (new optional fields and features negotiated in `Hello`/`Welcome`); the
  daemon never learns about UI concepts (layout, titles and the like stay in the GUI's
  saved session file); a GUI that needs a newer protocol says so and offers to restart the
  daemon only when no session would be lost. Handing sessions to a new daemon binary (PTY
  file descriptors passed over the socket with `SCM_RIGHTS`) is a later option.
- **Screen after a GUI restart:** the replay ring only holds recent bytes. For an exact
  screen the GUI saves a checkpoint of each pane's model on a controlled exit and
  attaches from the byte offset of that checkpoint (the checkpoint idea of section 9 and
  docs/MULTI_TERMINAL.md section 12); after a crash it replays the ring and accepts that
  older history is gone.
- **Development:** a separate socket name (`--daemon-socket`) so experiments never touch the
  daemon that holds real work.

Open: the framing format; whether the ring size should be larger for a local daemon (it is
a local resource, 256 KB is the browser default); idle exit time; Windows/macOS later.

## 11. Taking over terminals from the laptop in the browser (goal, owner 2026-11-22)

Goal: from a phone (browser) take over the terminals that run on the laptop (in the local
daemon of section 10), control them, and take them back later. Nothing here is built.

- **The server is a client of the daemon.** `matterm-server` can attach to the local
  daemon's sessions and offer them to browsers that logged in (password, as today). The
  wiring is explicit: an option such as `--expose-local-sessions` (name to be chosen
  clearly). Without it the server never talks to the daemon, and the daemon never
  listens on the network (its socket stays owner-only, section 10).
- **Private by default, shared on purpose.** A terminal is visible to the server only
  when it is marked shared ("Share this terminal" / "Stop sharing" in the palette, a mark
  in the tab while shared). The daemon enforces it; the server cannot list or attach to
  the others even with the right password. Least privilege beats one switch for everything.
- **No accidental exposure.** The server listens on loopback unless told otherwise; a
  non-loopback address needs its own explicit option and a warning at start; the plan of
  the owner is the local network or an SSH port forward. A terminal is a shell as the
  user: sharing it is remote code execution, so the password (Argon2id, rate limited),
  Origin check, session leases and the cookie rules stay as they are, and every take-over
  is logged (without screen contents).
- **Take-over is exclusive** (as the sessions of phase 1 already are): the new window
  becomes the one that writes and sets the size. The pane that lost the session shows
  that plainly (`TerminalView` already knows "taken over by another window" as a state):
  a one-line notice in the pane such as "This terminal is controlled from another window.
  Press Enter to take it back", the screen kept as it was, input paused, no resize. Enter
  attaches again from the replay offset and takes it back; the other window then shows the
  same notice. Nothing takes a terminal back by itself.
- **Size:** the window that has control sets the terminal size (a phone is narrow); on
  take-back the laptop resizes it again. Programs redraw on the size change.
- **Later:** a read-only attach (watch without typing, useful on a phone), and several
  windows viewing at once (mirroring, last input wins), with a visible "also open in N
  places" mark. Both are separate decisions.
- **GUI feedback:** the owner's window shows a mark in the strip for a terminal that is
  shared, and a notice when someone from outside took it over.

Open: the option names; whether sharing is per terminal or per tab; read-only first or
exclusive take-over first; idle expiry of a shared terminal.

### Decisions on sharing (owner, 2026-11-22, second round)

- The GUI shows whether a terminal is shared: a button at the end of the tab strip, grey
  ("Share") when nothing in the tab is shared and red when it is; the same toggle is in the
  command palette ("Share this tab", "Stop sharing this tab", "Share this terminal").
- A tab holds several panes. Proposal: the data model is per terminal (the daemon marks
  each session), the strip button acts on the whole active tab (all its panes), a shared
  pane has a small red mark in its corner, and the palette has the per-terminal entries for
  the exceptions. The button shows red when any pane of the tab is shared, with the count.
- The window that controls a terminal decides its size, also when several windows can
  watch it later (they get the controlling window's size).
- Option names (chosen by the integrator): `matterm-daemon` (the local daemon),
  `matterm-server --expose-local-sessions` (offer shared daemon sessions to browsers),
  `--listen ADDRESS` stays as it is (default loopback; a non-loopback address needs
  `--allow-remote-listen`, with a start warning), `matterm --daemon / --no-daemon` for the
  GUI, `--daemon-socket PATH` for a separate dev daemon.

### Styling (owner, 2026-11-22, to discuss later)
The owner wants some styling on the edges between terminals, in the palette and on the
edges of popups (today: flat colored borders made of cells). Ideas for the discussion:
a thin line border in cells with box-drawing or half-block glyphs (needs a cmap test),
a soft shadow under overlays, rounded corners with a small set of corner glyphs, a
different divider look for the focused pane (accent color), a subtle title row in popups.
Decide together with the theme/palette setting, not before.

## 12. Copy / scroll mode (owner request 2026-11-22; implemented as described)

A keyboard-only way to scroll back and select text, like tmux's copy mode. A **virtual cursor**
(never the program's real cursor, which is hidden while the mode is on) moves through the
retained text; the program gets no keys while the mode is on, and keeps running.

- **Enter:** prefix `[` (browser), palette "Copy / scroll mode", `Ctrl+Shift+Space` (desktop).
  Command `Command::CopyMode`, registry id `pane.copy-mode`. The layout reports it as
  `KeyResult/MouseResult::copyMode`; the owner of the view starts it in the focused pane.
- **Where the cursor starts:** on the real cursor when that is on screen, else the middle row of
  what is shown.
- **Keys (all consumed):** `h j k l` and the arrows; `0`/Home line start, `^` first non-blank,
  `$`/End last non-blank; `w b e` words; PgUp/PgDn and `Ctrl+B/F` a page, `Ctrl+U/D` half a page;
  `g`/`G` (or Ctrl+Home/End) oldest line / last line; `v` starts or ends a selection, Space starts
  one, `V` selects the whole (wrapped) line; moving extends it. `Enter` or `y` copies the selection
  (no selection: the line under the cursor) and leaves; `Esc`, `q` or `Ctrl+C` leave without
  copying. The view scrolls to keep the cursor in sight; the selection uses the same
  `Selection` as the mouse, so highlight and the 1 MiB copy limit are the same.
- **Shown:** the virtual cursor is a block at its cell, the selection is highlighted as usual, and
  a small badge `COPY` (with `SELECT` while selecting and `row/total`) sits in the pane's top right
  corner. The window or page title is not used.
- **Search:** `/` forward, `?` backward: type the pattern (shown in the badge, matches marked while
  typing), Enter runs it, Esc or Backspace on an empty pattern cancels the search (not the mode).
  `n` / `N` repeat in the same / the other direction, wrapping around the end ("search wrapped" in
  the badge; "not found: ..." when there is none). ASCII patterns, smart case (no capital: case is
  ignored), matches do not span rows. All matches in view are drawn in a highlight colour
  (`RenderFrame::matchRanges`); a search while selecting extends the selection.
- **Block selection:** `Ctrl+V` starts a rectangle between the anchor and the cursor (badge
  `BLOCK`, one `RenderFrame::selectionRanges` interval per row); `y` copies the columns of every
  row, trailing blanks trimmed, rows joined by newlines. `Ctrl+V` or `v` again ends it.
- **Not in it (yet):** marks, search across wrapped rows, non-ASCII patterns. Mouse still works as usual; a
  mouse selection made while the mode is on is simply the current selection.
- **Code:** `src/frontend/copymode.h` (no SDL; tests in `test/copymode_test.cpp`) holds the state
  and key handling; `TerminalView::copyMode` is the per-pane `std::optional`; both front ends
  feed it the keys that would have gone to the program and draw `apply()` + the badge.
- **Scrolling feel:** the wheel and touch scroll in pixels (a view may rest inside a row);
  `--snap-scroll` / `?snap=1` make it settle on whole rows after the input pauses.

## 13. Keyboard shortcut help and which-key (agent3)

- **Help view:** `ui::ShortcutsView` (`src/frontend/ui/shortcuts.h`), a scrollable layer listing
  every command that has keys, desktop and prefix columns, then Copy and Paste and the fixed keys
  of copy mode. Built from `buildCommandRegistry()`, so it cannot disagree with the keys. Opened
  by the palette entry **Keyboard shortcuts** (`app.shortcuts`), Ctrl+B `?` (browser) and
  Ctrl+Shift+H (native, `Command::ShowShortcuts`); `WorkspaceView` creates the layer itself.
  Up/Down/PgUp/PgDn/Home/End/wheel scroll, Esc, q or Enter close.
- **Which-key:** `ViewConfig::whichKeyDelayMs` (500, 0 = never) after the prefix key,
  `WorkspaceView::plan()` adds a small non-interactive layer at the bottom right listing the keys
  that can follow (`ui::WhichKey`, from the same bindings). `tick()` reports `changed` when it
  appears; `nextWakeMs()` includes the moment. It goes away with the next key or the prefix
  timeout.
- **Palette keys:** a long name is cut so that the keys are always shown; the key colour is
  lighter. Prefix keys read "Ctrl+B c" (the second key as typed), not "Ctrl+B C".
- **Daemon fallback (native):** when the daemon was wanted and cannot start, the first terminal
  prints a two-line note (`daemonFallbackNote`) before the shell, besides stderr.
- **Attach and detach (native, with the daemon):** palette entries *Attach terminal...* (a list
  of the daemon's terminals: folder, running command, "in another window", "shared"; ones this
  window shows are left out; choosing one opens a new tab on it, and one another window shows
  asks "Take over?" first, Cancel is the default), *Attach all leftover terminals* (only when
  there are terminals no window shows; asks, never automatic), *Detach terminal (keep running)*
  and *Move terminal to new tab* (the pane is closed with `endInDaemon = false`; a move attaches
  the same terminal in a new tab first). Entries and texts are in `daemoncommands.h`
  (`attachCommandEntries`, `attachItems`, `takeOverDialog`, `attachAllDialog`; tests in
  `test/daemoncommands_test.cpp`); the app glue (`daemonSessions`, `attachSession`) is in
  `app/ptyfrontend.cpp` and is not covered by a test (needs a window).
- **Session names:** a window's session is named after its first tab (made into a file name) until
  the owner chooses a name with *Save session as...* or opens a saved session; "default" while
  that tab has none. It is saved when a tab is named and at quit, and the file follows the first
  tab's name (the file of the old automatic name is removed). *Save session as...* offers the
  current name; the window title shows "| session <name>" once a session is being saved
  (`SessionPolicy::suggestion`, `named()`, `forget()`).
- **Delete session...** (native palette): a list of the saved sessions, then a question (Cancel is
  the default; it says so when it is the session this window saves into, which then is not
  written again until a tab is named). `src/frontend/sessioncommands.h`. The browser has no
  saved sessions, so nothing there.

## 14. Terminal in this pane, kill, and detached layouts (agent3)

Native with the daemon. Design in short:

- **Attach terminal here...** (palette, focused pane is a daemon pane): the same list as *Attach
  terminal...* (this window's own terminals left out), but the chosen terminal replaces the
  terminal of the focused pane (`NativePane::swapTerminal`: the view is reset and attached to the
  other session). A terminal another window shows asks "Take over?" first. Then one question about
  the old terminal: *Replace and end it* / *Replace, keep it running* (it stays in the daemon,
  detached, and is listed by Attach) / Cancel (the default). It names the running program when
  the pane is busy.
- **Kill terminal:** question (Cancel is the default; names the running program), then the pane
  is closed with its terminal ended. The last pane of the last tab closes the window, like any
  close.
- **Detach tab (keep terminals):** the active tab leaves the window, its terminals keep running in
  the daemon. The tab is saved as a *layout*: a one-tab `SessionSnapshot` in
  `$XDG_CONFIG_HOME/matterm/layouts/<name>.json` (`SessionFiles::defaultLayoutDirectory()`), where
  each pane also has `"terminal":"<32 hex id>"` (an optional schema field, validated like the
  browser's session ids) beside its folder and command. The name is the tab's name as a file name
  ("layout" without one; a free number is added when taken). No automatic restore, like sessions.
- **Attach tab...** (earlier "Attach layout..."; the internal ids and the layout files keep that word): a list of the saved layouts; choosing one opens it as a new tab with the
  saved splits and ratios. For each pane the daemon is asked (`planLayoutAttach`): its terminal is
  free: attached again; another window shows it: one question first ("Take them over?");
  missing, gone or already shown here: a new terminal in the saved folder, with a note in the pane.
  The layout file is removed once it is attached (it is not detached any more). Detaching again
  makes a new one.
- Code: `daemoncommands.h` (entries, dialogs, `planLayoutAttach`), `sessionsnapshot.h`
  (`SavedPane::terminal`), glue in `app/ptyfrontend.cpp` (no test: needs a window).

## 15. Work indicator on the tabs (agent3)

- A tab is **working** when any pane of it had output in the last 3 s (`TabPulse::kWorkingMs`),
  also the tab in focus. "Output" is the growth of `TerminalView::outputBytes()`, polled by
  `WorkspaceView::tick(nowMs)` (`OutputWatch`, `src/frontend/tabpulse.h`): no hooks in the
  frontends, the same code natively and in the browser, exact in tests (the caller gives the time).
- A working tab's background is its usual colour in the even seconds and 16 % darker (towards
  black, `kDarkenPercent`) in the odd ones: `TabPulse::dark(nowMs)` is one **global** phase, so all
  working tabs flip together. This holds for the active tab too (its light colour to a slightly
  darker one). The ● mark for "something happened while you were away" is unchanged.
- Cost: the strip's cells change at most once a second and only while a tab works;
  `tick()` reports `changed` only when the pulse state flips, `nextWakeMs()` names the next phase
  boundary only while a tab works (nothing else is scheduled), and the frame hash decides about
  drawing as before (docs/CPU.md). Measured with `MATTERM_LOOP_STATS=1`, a window with one tab and
  a program printing every 0.4 s draws the same number of frames with and without the pulse (36 and
  35 in 8 s): the strip is hidden with one tab, so this shows only that nothing else was added.
- Off: see "Mode" below (`--tab-activity=off`, `--no-tab-pulse`, `?pulse=0`).
- Tests: `test/workspaceview_test.cpp` (working or not, phase, the time limit, in step, off, the
  wake-up), `web/tools/e2e-pulse.js` (pictures of the strip; not run here: no browser).
- **Your own input and new panes do not count** (`OutputWatch`): output within 5 s after something
  was sent to the pane (`TerminalView::inputBytes()`: keys, paste, mouse reports) is the echo or a
  program answering you, and output within 5 s after a pane is first seen (a new terminal, pane,
  tab, or the replay of an attached one) is its startup. Both are `ViewConfig::pulseIgnoreAfterInputMs`
  and `pulseIgnoreAfterStartMs` (5000; the owner said 5-10 s, longer than the round trip of an echo
  through a tunnel). Output after that starts the pulse as usual. Program answers to its own
  queries (device attributes) also count as input, which only makes the pane calmer.

### Mode: icon (default), pulse, off

The owner preferred a static mark to the blinking background, so `ViewConfig::tabActivity`
(`TabActivity`, tabtypes.h) chooses: **icon** (default), **pulse** (the alternating background
described above) or **off**. Flags until there is a config file: `--tab-activity=icon|pulse|off`
(native; `--no-tab-pulse` stays as an alias for off), `?tabactivity=icon|pulse|off` (browser;
`?pulse=0` is off).

- **Icon:** a tab that works (same rule as above, same ignore of your own input and of new panes)
  shows `kWorkingIcon` U+25C9 `◉` in the mark slot before its title, green (darker green on the
  active tab's light background). It is not the amber `●` that says "something happened while
  you were away"; when both apply, the working icon is shown. The icon is looked at once a
  second (`WorkspaceView` evaluates it when the second of `tick(nowMs)` changes), so it appears or
  disappears at most once a second and never animates; `tick()` reports `changed` only then, and
  `nextWakeMs()` names the next second only while the icon shows or a tab works. The glyph is in
  the bundled font (`kStripGlyphs`, cmap test); `⚙` and `◐` are not, hence this one.
- **Tests:** `test/workspaceview_test.cpp` (the icon shows after the grace, exactly two changes
  over eight seconds of ticking every 50 ms, own input silences it, the wake-up, the mode names, off
  leaves no timer); the pulse tests set the mode with `setTabActivity`.

## 16. The native window's own frame (agent3; decided by the owner, 2026-11-22)

The native window draws its own frame: the tab strip row is the title bar. This is the **default**;
`--system-frame` asks for the desktop's frame instead, `--borderless` is still accepted (it is the
default). The web client is unaffected (the browser has its own frame; the buttons are never offered).
Where our frame cannot work, the system frame is used and a note says so (below).

**Strip.** The strip ends with `[ – ][ □ ][ ✕ ]` (three columns each, after the Share and palette
buttons): minimise, maximise (`◫` while maximised: the bundled font has no overlapping-squares glyph)
and close. They are `StripHit` kinds (`Minimize`, `Maximize`, `CloseWindow`);
`WorkspaceView::mouse()` turns a click into `MouseResult::action` `window.minimize`,
`window.maximize` (toggle) or `window.close`, which the native loop executes with SDL. Close is
`requestQuit()`: the same close question as the window's own close button. A double click on a gap
of the strip (a column with no region) also gives `window.maximize` (SDL may swallow that click on
X11; the button always works). `ViewConfig::windowButtons` turns the buttons on. The glyphs are in
`kStripGlyphs`, so the font test covers them.

**Hit test.** `SDL_SetWindowHitTest` asks, for a point in window coordinates, whether it moves or
resizes the window. The decision is pure (`src/frontend/windowhit.h`, `classifyWindowPoint`): the grip
`kResizeEdge` 8 px, corners `kResizeCorner` 16 px along the edge, none when maximised; then the strip
row over a gap column (`dragSpans`) is the drag handle; everything else is normal, so tabs and
buttons get their clicks. SDL may ask from another thread: the callback reads a mutex-guarded copy of
the geometry that the main loop refreshes each frame (`app/borderless.h`). `SDL_WINDOW_RESIZABLE` is
set at creation and again after (Wayland ignores resize hits otherwise). The top 8 px of the window
are grip too although nothing is drawn there.

**Border.** `kBorderWidth` 4 px on the left, right and bottom, none at the top (the strip is the top
edge) and none when maximised (`windowInset`, `borderRects`). It is **outside** the content: the view
(strip, panes, dividers, layers) is laid out in the window minus the inset, so nothing is covered.
The 8 px grip is the 4 px border plus 4 px of content next to it; **those inner 4 px are grip, not
border, and not drawn**. The colour is the strip's own background (`borderColor` =
`tabstrip_detail::colors(palette).stripBg`, the fill of the part of the strip that is dragged).

**Pointer.** Over the grip the pointer shows the system resize shapes (`SIZEWE`, `SIZENS`, `SIZENWSE`,
`SIZENESW`; `cursorShapeFor(zone)` maps the same classification as the hit test). Not in a maximised
window; leaving the zone or the window restores the pointer the program used. Without a cursor theme
SDL gives no system cursor: one stderr line, the pointer stays an arrow.

**Fallback: no half modes.** The window is created borderless; if `SDL_SetWindowHitTest` fails (the
dummy driver and old SDLs say "not supported") the frame is given back (`SDL_SetWindowBordered(TRUE)`),
the buttons stay off, and the line `matterm: the built-in window frame is not available (<reason>);
using the system frame (--system-frame hides this note)` goes to stderr and to the first terminal
(`chooseFrame()`, tested). Without the hit test a borderless window could be neither moved nor
resized. On GNOME Wayland with SDL 2.30 the hit test works (the owner's first test), which is why the
own frame is the default there.

**`matterm --window-test`.** Opens a small window and prints the SDL video driver, the session
environment and one line per call: create borderless, resizable kept, hit test, bordered on and off,
maximise, restore, minimise, restore. `ok`, `called, state not reported` (a call that returns nothing
and whose state SDL cannot show: Wayland reports no minimised state; not a failure) or `FAILED: <SDL
error>` (an error return). The last line says whether the own frame would be used here.
`MATTERM_HITTEST_LOG=1` prints each change of the hit test's answer (`hit test (x, y): zone N`, 2..9
resize, 1 drag) for desktops where resizing does not work. `MATTERM_ASSUME_HIT_TEST=1` is a test
hook: it takes the own frame as if the hit test had worked (the dummy driver has none), used by the
ctest `pty_frontend_tabs_smoke_borderless`.

**Decided against** (owner, 2026-11-22): SDL3; a transparent window with rounded corners or a shadow
(the owner's own alpha test on GNOME Wayland showed no transparency, also with
`wl_surface.set_opaque_region(NULL)`; SDL2 has no transparent-window flag and a client-side shadow
would need libwayland-client / libX11); detecting a missing libdecor frame (the own frame being the
default makes it moot). Nothing of these is in the code.

**Tests.** `test/windowhit_test.cpp` (zones for every pixel, drag spans, the inset equals what the
border leaves, the border colour equals the strip's, cursor shapes, the frame request flags and the
fallback), `test/tabstrip_test.cpp` (the buttons at every width), `test/workspaceview_test.cpp` (clicks,
double click, restore mark), `test/clioptions_test.cpp` (the flags and the completion).

## 17. Settings and themes (agent2; decided by the owner, 2026-11-23)

**The file.** `$XDG_CONFIG_HOME/matterm/settings.json` (else `~/.config/matterm/settings.json`), on the computer the window runs on,
also with `--daemon-host`. Strict JSON (`src/frontend/jsonvalue.h`: duplicate keys, bad UTF-8, lone surrogates, too deep or over
1 MiB are refused), written atomically (a temporary file next to it, synced, mode 0600, renamed; the folders 0700). Keys this version
does not know are written back as they were. The definitions (key, text, type, default, allowed values) are in
`src/frontend/settings.h`:

| key | values | default |
|---|---|---|
| `theme` | grey, blue, green, red, amber | grey |
| `computers` | `{"<name>": "<theme>"\|"auto"}` | `{}` |
| `tabActivity` | icon, pulse, off | icon (the command line wins where given) |
| `windowFrame` | own, system | own (read at start; the command line wins) |
| `scrollbackRows` | 100..100000 | 1000 (terminals made afterwards) |
| `confirmClose` | true, false | true |

A value that does not fit is a warning and its default is used; a file that is not JSON is reported (in the window title) and the
values from before stay; saving over a broken file is refused.

**Following the file.** `SettingsWatcher` reads the file at start and looks at it about once a second (one `stat`; the file is read
only when its time or size changed), pure given a file system and a clock. The palette entry "Reload settings" reads at once.
inotify can come later; polling stays the baseline.

**Themes** (`src/frontend/themes.h`). Dark only. A theme colours the frame (tab strip, its marks, the window border, which is the
strip's colour: `Palette::frame`) and tints the terminals' default background by 5 % (`TerminalView::setDefaultBackground`);
programs' own colours are not touched. The themes differ in **hue only** (about the brightness of grey, today's look; tests keep the
hues apart and the text on the strip and on the terminal readable). Colour is not the only sign of where a window is connected: for
terminals on another computer its **name is written in the strip's gap** (between the last tab and the share button), muted in the
theme's colour, not in the tab's name (`placeComputerName` in `tabstrip.h`, the one place to change when it moves).

**Per computer.** The local computer has the user's theme. A computer reached with `--daemon-host NAME` (the part after `user@`)
has its override from `computers`, else an automatic choice: a hash of the lower-cased name picks among the themes other than the
local one, so the same computer always gets the same theme and a remote window never looks like the local one. Two computers may
get the same theme; an override fixes that. Detecting a plain `ssh host` typed in a shell is later.
