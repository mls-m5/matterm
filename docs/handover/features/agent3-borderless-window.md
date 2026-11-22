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

## Update 2026-11-22 (after the owner's test on GNOME Wayland, SDL 2.30)

Decided by the owner: system frame only when the hit test fails; restore glyph `◫`; edge 6 / 12 px.

- **Probe wording.** `--window-test` says `called, state not reported` (not FAILED) for a call that
  returned and whose state SDL cannot show; Wayland reports no minimised state. The owner's output
  was: SDL 2.30, hit test ok, `SDL_MinimizeWindow` FAILED (that is this case). I did not receive his
  full text, so it is not copied here. My Wayland assumption (hit test unsupported) was wrong for 2.30.
- **Edge resize.** Logic checked for every pixel; `SDL_WINDOW_RESIZABLE` also set explicitly. Probable
  cause was that no edge was visible. Check again with the border; if it still fails, run
  `MATTERM_HITTEST_LOG=1 matterm --borderless` and move the pointer over the edges and report the
  `hit test (x, y): zone N` lines (2..9 resize, 1 drag) and what happens when you press and drag.
- **1 px border** in a theme colour, not when maximised. Check: it should be visible all round.
- **Rounded corners / shadow:** only investigated, three options and a recommendation in TABS_PLAN 16.2.
- **No system frame (libdecor without a plugin):** three ways of detecting it, chosen with
  `MATTERM_FRAME_DETECT=off|borders|plugins|message|any` (default `off`, nothing changes). To try:
  1. `matterm --window-test` in the setup *without* the libdecor plugin and again *with* it (a
     session where the window has a frame). Report the block "does the system draw a frame?": the
     borders SDL gives, the number of plugin libraries, whether libdecor complained, and what each
     variant concludes.
  2. For each of `borders`, `plugins`, `message`, `any`: `MATTERM_FRAME_DETECT=<v> matterm` with no
     plugin. Expected: the window gets the built-in frame and a line in the first terminal. With the
     plugin: it must keep the system frame. Report which variants did the right thing in both setups.
  Then I keep the one that works and delete the others.
- Also fixed: clang-tidy `bugprone-misplaced-widening-cast` in `ui/commandpalette.h` and `ui/shortcuts.h`
  (not run: no clang-tidy here; the casts now widen each operand before the arithmetic).
- Not verified: all of the above on a real display; `StderrCapture` was checked with a small program only.

## Update 3 (owner's third round)

- **Border / grip:** visible border 4 px (outermost pixel brighter), grip 8 px, corners 16 px (inside
  the window). It is drawn over the content (up to 4 px of the edge cells and the strip's top row are
  covered); insetting the layout instead is not done. If resize still fails, run
  `MATTERM_HITTEST_LOG=1 matterm --borderless 2> hit.log`, move the pointer slowly to the edge and
  into the corner, press and drag, and send `hit.log`. Lines look like `hit test (x, y): zone N`
  (N = 2..9 resize, 1 drag, 0 none). No lines at the edge at all means SDL does not ask there.
- **Transparency probe** in `matterm --window-test` (after the existing lines): three windows of
  about two seconds each (software renderer, accelerated renderer, OpenGL with alpha). Please run it
  on GNOME Wayland and, with `SDL_VIDEODRIVER=x11 matterm --window-test`, on X11, and report for
  each of the three windows whether you see the desktop through the red margin (and through the
  outer rim), plus the printed lines. Details: TABS_PLAN 16.4.
- **Window geometry / `_GTK_FRAME_EXTENTS`:** only the handles from `SDL_GetWindowWMInfo` are
  printed. Setting them would need libwayland-client or libX11 (a link dependency), so I stopped:
  question below.
- **cert-err33-c** in `app/borderless.h` (5 places): the results are now cast to `(void)`.
- Not verified: everything on a real display; the OpenGL and accelerated variants (the sandbox has
  neither); clang-tidy.

## Open design questions (the owner decides)

(1-3 of the first round are decided, see above.)
1. Rounded corners: nothing / painted corners / wait for SDL3 (TABS_PLAN 16.2; recommended: nothing, painted corners if wanted).
3. May `--csd` link libwayland-client / libX11 to set the window geometry and `_GTK_FRAME_EXTENTS`? Without it no
   compositor-correct shadow is possible (TABS_PLAN 16.4). Alternatives: yes / no, drop `--csd` and keep the plain
   border (recommended unless the probe shows transparency works and you want the shadow).
2. Detecting a missing frame: which variant, or a `windowFrame = system | own | auto` setting (TABS_PLAN 16.3).
