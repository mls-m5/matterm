# Borderless window (`--borderless`, `--window-test`)

Written by agent3, 2026-11-22. Design: `docs/TABS_PLAN.md` section 16. Contract: `docs/tasks/borderless-window.md`.

## What changed

- `matterm --borderless`: the window has no system frame. The tab strip row is the title bar:
  - drag the empty part of the strip (between the last tab and Share) to move the window;
  - drag the window edges and corners to resize it;
  - `–` minimise, `□` maximise (`◫` restore while maximised), `✕` close at the right end of the strip;
  - `✕` asks the same close question as the window's normal close button;
  - a double click on the empty part of the strip toggles maximise (see "not verified").
- If the desktop cannot do this (SDL says "not supported", e.g. native Wayland), the system frame is
  restored, one line goes to stderr and the same line is written in the first terminal:
  `matterm: --borderless is not available (<reason>); using the system frame`. No half mode.
- `matterm --window-test`: opens a small window for a moment and prints the SDL video driver, the
  session environment variables and one `ok` / `FAILED` line per window call (borderless, hit test,
  bordered on/off, maximise, restore, minimise). The last line says whether `--borderless` would work.
- Default is unchanged (system frame). The web client is unchanged.

## How to check it (the owner, on GNOME)

1. `matterm --window-test` and send me the output. On GNOME Wayland I expect `SDL_SetWindowHitTest ...
   FAILED` and the fallback line. Try `SDL_VIDEODRIVER=x11 matterm --window-test` too (XWayland).
2. `matterm --borderless`, or `SDL_VIDEODRIVER=x11 matterm --borderless`. Check: moving, each edge and
   corner, the three buttons, double click on the strip, closing with a program running (the question),
   maximised window (no resize edges), a high-DPI screen (the drag area should line up with the gaps).

## Where the code is

- `src/frontend/windowhit.h`: pure logic (`classifyWindowPoint`, `dragSpans`, `chooseFrame`), tested in
  `test/windowhit_test.cpp`.
- `src/frontend/tabstrip.h`: the buttons (`WindowButtons`, `StripHit::Minimize/Maximize/CloseWindow`).
- `src/frontend/workspaceview.h`: `ViewConfig::windowButtons`, actions `window.minimize|maximize|close`.
- `app/borderless.h`: the SDL hit-test callback and a mutex-guarded copy of the geometry.
- `app/ptyfrontend.cpp`, `app/main.cpp`: window flag, fallback, actions, `--window-test`.

## Verified

- Build without warnings; ctest 85 of 85 green (soak included), with new tests for hit logic, strip
  buttons at every width 0..200, view clicks and double click, the font having the new glyphs.
- `--window-test` and the tabs smoke test with `--borderless` under the SDL dummy driver: the hit test
  is "not supported" there, and the fallback path (frame given back, line on stderr) works.

## NOT verified (no display here)

- Everything with a real window manager: dragging, edge resize, cursor shapes at the edges, the buttons
  against a real frame, maximise/restore state, high-DPI scaling of the drag area.
- Whether SDL delivers a double click on a draggable area at all. On X11 SDL starts the move itself and
  may swallow the click, so the double click may not work there; the maximise button always does.
- Behaviour on Wayland beyond "SDL 2.26 reports the hit test as unsupported" (assumed from the SDL source).
- `--borderless` with `--daemon-host` / the daemon (the window code does not depend on it).
- clang-format: the new files are formatted; `app/main.cpp` and `app/ptyfrontend.cpp` were not
  reformatted (shared files, not clean before), my lines follow the local style.

## Open design questions (the owner decides)

1. Wayland: fall back to the system frame (done; recommended) / try XWayland automatically / refuse to start.
2. Restore glyph: `◫` (the font has no overlapping squares) / `▫` / draw with ASCII `[]`.
3. Resize edge 6 px, corner 12 px: enough on GNOME, or wider?
