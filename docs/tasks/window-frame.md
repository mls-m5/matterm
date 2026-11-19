# Task: a framed, narrower terminal window in the browser

Assignment for an independent agent in its own worktree (`.tmp/agent3`, new branch
`agent3-frame` from `main`). Read first: Agents.md, docs/PARALLEL_WORK.md,
web/README.md, `web/shell.html`, `web/style.css`, `web/login.js` (`fitCanvas`,
`setupTouchKeyboard`), and `web/main.cpp` (how the canvas size drives columns and
rows, `fontPixels`, the idle throttling in `frame()`).

## Owner's wish

The terminal should not always be full screen. By default it is a window-like
panel on the page: narrower than the viewport, with margins on all sides, a soft
shadow and rounded corners all around. The terminal is *embedded in a window
frame*, not edge to edge.

## Scope

1. **Default layout on desktop** (pointer: fine, wide viewport): the canvas sits in a
   container centered on a dark page background with margins (a sensible default
   such as `width: min(100% - 2 * margin, 1100px)`, height with the same margin),
   rounded corners (about 10-12 px), a layered soft box-shadow and a hairline
   border. The container clips the canvas to the rounded corners
   (`overflow: hidden`), including the scrollbar strip and the selection overlay.
   Keep the terminal background and the page background clearly distinct.
2. **Phones and tablets, and small viewports**: stay edge to edge (no margins, no
   radius), because space is scarce; the key bar and soft keyboard keep working
   (`body.touch`). Use a CSS media query on width/pointer, not user-agent checks.
3. **Full-screen toggle**: a small control (and the `?fullscreen` query parameter,
   and F11-like handling is NOT needed) switches between the framed and the
   edge-to-edge layout; remember the choice in `localStorage` (try/catch). It must
   not capture terminal keys: pick a control that is mouse/touch only, hidden
   until the pointer is near the frame's top edge or the toggle is focused, so it
   does not cover terminal content.
4. **Optional slim title bar** inside the frame showing the terminal title (the
   `document.title` text already set by `main.cpp`; do not add new exports unless
   needed). Keep it off if it costs terminal rows in an awkward way; document the
   decision. Never render page text from the program as HTML (use `textContent`).
5. **Sizing correctness**: the grid must be computed from the *container* size, not
   `100vw/100vh`: update `fitCanvas`, the CSS and any `main.cpp` code that reads
   the window size, so columns/rows match the visible canvas, HiDPI stays crisp,
   resize and device rotation work, and the server gets the right size through the
   existing resize path. The idle throttling must keep working (no per-frame
   cost added; wake on resize).
6. **Theme-awareness**: the frame colors follow `prefers-color-scheme` for the
   page background only; the terminal palette is unchanged.
7. **Embedding on another page** (`<iframe>`) is out of scope and stays blocked by
   the server's headers; mention in `web/README.md` that "embedded" here means
   the framed window, and do not change `frame-ancestors`/COOP/CORP.

## Tests

Extend `web/tools/` (Playwright, see `e2e-touch.js`, `run-e2e.sh`): at a desktop
viewport the canvas bounding box has margins on all sides and is narrower than the
viewport; the container has a non-zero border-radius and box-shadow; at a phone
viewport (`isMobile`, `hasTouch`) the canvas is edge to edge; the toggle switches
layouts and survives a reload; `stty size` matches the visible canvas size within
one cell in both layouts; existing e2e scripts still pass. Add the new script to
`run-e2e.sh`. Take a screenshot of the framed layout into the report's output
directory.

## Owned and shared paths

Owned: `web/style.css`, `web/shell.html`, `web/login.js`, `web/tools/*`,
`web/README.md`. Shared with the integrator, edit minimally and report:
`web/main.cpp` (only where the canvas/window size is read). Do not touch
`src/server/`, the protocol, or terminal model code.

## Acceptance

Full ctest suite passes (`-DMATTERM_BUILD_SERVER=ON`); the web build
(`emcmake cmake -S web -B <dir> -DMATTERM_FONT=...`) and all of `run-e2e.sh` pass
when the tools exist in your sandbox (say plainly what you could not run); small
commits; a short report with the screenshot path and anything unverified on real
devices. Do not push.
