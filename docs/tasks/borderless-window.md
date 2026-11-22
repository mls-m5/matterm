# Borderless native window (agent3)

Goal: `matterm --borderless` draws its own window frame: the tab strip row is the window
handle. Default stays the system frame.

Scope
- Write a short design first as `docs/TABS_PLAN.md` section 16, then implement.
- Strip: minimise, maximise/restore and close buttons at the right, a draggable gap between
  the last tab and Share. Close uses the existing close question.
- Edge resize and drag through `SDL_SetWindowHitTest`; double click in the drag area toggles
  maximise.
- If borderless or the hit test fails (for example Wayland), fall back to the system frame
  with a log line and a notice. No half modes.
- `matterm --window-test`: prints the SDL video driver and which calls worked, for the owner
  to run on GNOME.
- Pure hit-test/region logic in a header with unit tests.
- Document GNOME/Wayland differences. The web client is unaffected.

Out of scope: a `windowFrame` setting (later), changes to panes or the daemon.

Owned paths: `src/frontend/tabstrip.h`, `src/frontend/workspaceview.h`, new header for hit
testing, `src/frontend/clioptions.h`, `docs/TABS_PLAN.md`. Shared: `app/main.cpp`,
`app/ptyfrontend.cpp` (agent2 owns the style; do not reformat wholesale).

Acceptance: ctest green, tests for the hit-test logic, warning-free, clang-format run.
The GUI cannot be run in the sandbox: say so; the owner checks with `--window-test`.
