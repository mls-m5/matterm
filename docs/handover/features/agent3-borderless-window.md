# The native window's own frame (default), `--system-frame`, `--window-test`

Written by agent3, 2026-11-22, rewritten after the owner's decisions. Design: `docs/TABS_PLAN.md`
section 16. History of the rounds is in git (`docs/handover/features/agent3-borderless-window.md`).

## What it is now

- `matterm` draws its own window frame **by default**: the tab strip is the title bar.
  - Drag the empty part of the strip to move the window; double click there toggles maximise.
  - `–` minimise, `□` maximise (`◫` while maximised), `✕` close (asks the usual close question).
  - Resize at the edges and corners (grip 8 px, corners 16 px). The pointer shows resize shapes there.
  - A 4 px border in the strip's own colour on the left, right and bottom (none at the top, none when
    maximised). It is outside the content: the layout is inset, nothing is covered. The inner 4 px of the
    grip are grip only, not drawn.
- `matterm --system-frame`: the desktop's frame instead. `--borderless`: still accepted, same as the default.
- If SDL's hit test fails (no way to move or resize): the system frame comes back, and a note goes to
  stderr and the first terminal. No half modes.
- `matterm --window-test`: SDL driver, environment and one line per window call
  (`ok` / `called, state not reported` / `FAILED: <error>`), last line says what would be used.
- `MATTERM_HITTEST_LOG=1`: prints each change of the hit test's answer. `MATTERM_ASSUME_HIT_TEST=1`: test hook.
- The Share button shows `⇧` (see `agent3-share-symbol.md`).
- Removed (the owner decided against them): detecting a missing libdecor frame (`MATTERM_FRAME_DETECT`),
  the transparency probe, anything about shadows, rounded corners or `--csd`.

## How the owner checks it (GNOME Wayland, then X11 with `SDL_VIDEODRIVER=x11`)

1. `matterm`: no system frame, the strip on top, the border on three sides; maximise removes the border and
   the layout fills the screen; the first and last text columns and the bottom row are complete.
2. Move by dragging the strip's gap; resize at each edge and each corner; check the pointer shapes (they
   should change at the same pixels where a press starts a resize) and that it is the arrow again over the
   text and on the strip.
3. The three buttons; closing with a program running asks first.
4. `matterm --system-frame`: the desktop's frame, nothing of ours.
5. If resizing or the pointer shapes fail: `MATTERM_HITTEST_LOG=1 matterm 2> hit.log`, move to the edge and
   into a corner, press and drag, and send `hit.log`; also the stderr line `no resize pointer shape from the
   desktop (...)` if there is one. `matterm --window-test` output helps on a new desktop.

## Where the code is

- `src/frontend/windowhit.h`: pure logic (zones, drag spans, inset, border, colour, cursor shapes, frame
  request flags, fallback choice), tested in `test/windowhit_test.cpp`.
- `src/frontend/tabstrip.h` / `workspaceview.h`: the buttons, actions `window.minimize|maximize|close`.
- `app/borderless.h`: SDL hit test callback and the geometry copy. `app/ptyfrontend.cpp`, `app/main.cpp`: the
  window flags, fallback, inset layout, pointer shapes, `--window-test`.

## Verified

Build without warnings; ctest 88/88 (soak included). New tests for the zones (every pixel), the inset against
the border, the colour against the strip, cursor shapes, the flags and the fallback; the whole tabs smoke
script runs under the dummy driver both with the fallback (default there) and with the own frame and the
inset layout (`pty_frontend_tabs_smoke_borderless`).

## NOT verified (no display here)

Everything with a real window manager and real pointer: moving, resizing, the buttons against a real desktop,
pointer shapes on Wayland, high-DPI scaling, double click on the handle (SDL may swallow it on X11),
X11. clang-tidy (not installed here). clang-format was not run on the shared files `app/main.cpp` and
`app/ptyfrontend.cpp` (not clean before); the new files are formatted.
