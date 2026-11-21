# Current handoff

Last updated 2026-11-21. `main` is pushed up to `128707c` (plus the chat design in
`docs/CHAT.md`, merged locally). See `ROADMAP.md` for the long-term list and
`docs/PARALLEL_WORK.md` for who works on what.

## State

- Native terminal, browser client, server (with server-owned sessions, replay ring,
  take-over, logout/close-all), and the Mattrix OS demo (web, native window,
  `matterm-demo --tty`, standalone `mattrix`) are done and on GitHub. The demo is
  published to GitHub Pages by `.github/workflows/pages.yml`.
- Warnings and hardening: `cmake/Warnings.cmake`; clang-tidy: `.clang-tidy`,
  `tools/clang-tidy.sh [--changed]`, weekly run in `.github/workflows/clang-tidy.yml`
  (untested on GitHub). See `docs/STATIC_ANALYSIS.md`. The tree builds warning-free.
- Verification commands: `cmake -S . -B build && cmake --build build && (cd build &&
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest -j4)`, server build in
  `build/server` (`-DMATTERM_BUILD_SERVER=ON`), web in `build-web`
  (`emcmake cmake -S web -B build-web`), browser checks with
  `NODE_PATH=$HOME/pw/node_modules sh web/tools/run-e2e.sh <matterm-server> build-web/site`.
  Do not put `===` inside `EM_ASM` (clang-format splits it); `long` is 32 bits in wasm.

## Open work

- Chat for Mattrix OS (`docs/CHAT.md`, agent3): ChatRoom core, codec, ChatClient,
  `chatd`, web bridge, Podman, `mattrix-chat`. Design agreed; build order in section 9.
- Tabs, panes and chrome in cells: planned in `docs/TABS_PLAN.md` (decisions in
  `docs/MULTI_TERMINAL.md` section 13); not started. Earlier: browser tabs and terminal views were paused. The finished implementation (TerminalView,
  TabModel, tab bar, two split panes) is on `archive/web-tabs`; `main` keeps only the
  design docs (`docs/tasks/web-sessions.md`, `docs/MULTI_TERMINAL.md` section 5).
- Width reflow (`docs/SCROLLING_AND_REFLOW.md` lists what is open).
- Unverified on real hardware: native window snapping, Termux, Podman/nginx deployment,
  sound in a browser and in the SDL window.
- Old unmerged branches kept for reference: `agent2-fastjson`, `agent2-fuzz`,
  `agent2-sessions`, `agent3-resize`, `agent3-selection`, `agent3-url`,
  `archive/agent3-resize-59308b7`. Check before deleting; the work was most likely
  integrated by cherry-pick.

## To investigate (reported by the owner, 2026-11-21)

- Native window: in a fairly new window, while the user resized it, the window
  jumped back and forth as if something kept triggering a resize (possibly a
  resize loop between the window snapping/size rounding and the window manager).
  Not reproduced yet; no details on the compositor or the steps. See
  `src/frontend/windowsnap.h`.
