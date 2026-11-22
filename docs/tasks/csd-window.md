# Experiment: own shadow and rounded corners (`--csd`) (agent3)

Goal: find out if `matterm --csd` can draw what GTK/gedit draws itself (client-side
decorations): a soft shadow and rounded corners around the window, with our strip as the
title bar. The owner dislikes extra dependencies: **no new libraries** (SDL, plus Xlib and
libwayland only through what SDL already exposes via `SDL_GetWindowWMInfo`; if a link
dependency would be needed, stop and report instead).

Depends on: the borderless window (docs/tasks/borderless-window.md) with its fixes (resize,
1 px border, minimise probe). Start after those are merged.

## Steps (stop and report after each; the owner tests on GNOME Wayland and maybe X11)
1. Extend `matterm --window-test` to probe: can the window be transparent (alpha in the
   framebuffer: which renderer and what it needs; X11 needs a compositing manager and an
   ARGB visual), and can we set the window geometry (Wayland `xdg_surface` from WMInfo) or
   `_GTK_FRAME_EXTENTS` (X11)? Print one line per probe like the existing ones.
2. If the probes pass: draw a transparent margin with a shadow and rounded corners behind the
   content, resize edges in the margin, margin removed when maximised/tiled/fullscreen, window
   geometry/frame extents set so the compositor snaps correctly. Keep it in separate files
   (`app/csd.*`, pure geometry in a tested header); the rest of the code only gets small hooks.
3. Measure CPU with docs/CPU.md's method; the shadow must be cached, not redrawn every frame.

## Fallback chain (decided by the owner)
1. `--csd` works: keep it (still opt-in until the owner says otherwise).
2. It does not work, or costs too much: remove `app/csd.*` and its hooks; `--borderless` keeps
   the plain 1 px border; the system frame (libdecor) stays the default.
3. At run time `--csd` falls back to plain `--borderless` when transparency is unavailable.

Out of scope: the web client, new settings, anything in the daemon.
Acceptance: tests for the geometry, warning-free, clang-format, a feature description in
`docs/handover/features/`. State plainly what could not be seen (no display in the sandbox).
