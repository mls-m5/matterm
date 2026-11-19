# Current handoff

## Repository state (2026-11-19)

- `main` was clean at `db2cb22` before this handoff. That commit updates only
  the `lib/fast-json` gitlink to `5544003bfa2b909911d52131da657bb4a2fb07bb`.
  The operator pushed that fast-json commit manually; its presence on GitHub
  was verified. The locally stored `origin/main` for matterm was `8576897`.
  This handoff and the gitlink update have not been pushed from this session.
- Agent2's remaining fast-json integration is `fe6e5e6` (remove the server
  config syntax workaround and update tests), then `3dd6866` (docs and
  roadmap). Review and apply them if continuing that task; do not reapply
  `b2bcae8` (gitlink) or cherry-pick the branch's merge commit. Preserve the
  unrelated roadmap entries if resolving conflicts. The worktree is
  `.tmp/agent2` (`agent2-fastjson`).
- Agent3's CLI help and smooth-cursor documentation are integrated as `c9ae518`.
  The `Text has zero width` fix is integrated as `8576897`; U+00AD reproduces
  the SDL_ttf error, but the original session's exact trigger is unknown.
  The native suite passed 40/40 after that fix. `.tmp/agent3` is still on
  the original fix commit `5c73679`; its patch is already on `main`.

## Next milestone: browser-to-server test

The browser client, server, login and PTY bridge already exist. The server
was built in `build/server/` and its 24 tests passed with the new gitlink.
This environment had no `emcmake` or `build-web/site/`; a full browser login
and interactive shell were not verified in this handoff.

1. On a machine with the Emscripten SDK, build the client per
   [web/README.md](web/README.md):

   ```sh
   emcmake cmake -S web -B build-web \
     -DMATTERM_FONT=build/assets/fonts/JetBrainsMono-Regular.ttf
   cmake --build build-web
   ```

2. If needed, build the server using [docs/SERVER.md](docs/SERVER.md). Create
   credentials as a non-root user with the documented `--set-password` flow
   (password via stdin). Serve the generated directory from the same server:

   ```sh
   build/server/src/server/matterm-server \
     --credentials "$HOME/.config/matterm/credentials" \
     --static-dir build-web/site --origin http://localhost:8080
   ```

3. Open `http://localhost:8080/`, log in, run a command, resize the browser
   and verify that exiting the shell returns to login. Keep the server on
   loopback for this trial. For later remote use, validate HTTPS/WSS via a
   reverse proxy and match the server's allowed origin to the public URL.
   Do not assume the provisional Podman deployment has been validated.

See [ROADMAP.md](ROADMAP.md) for deferred work: terminal resize/scrollback,
performance baselines, session multiplexing and custom window chrome.

# Historical handoff: smooth scrolling / cursor investigation

Latest agent3 handoff: the SDL_ttf `Text has zero width` investigation and fix
are documented in [docs/tasks/zero-width-glyph.md](docs/tasks/zero-width-glyph.md).
The older smooth-cursor/help work below has already been integrated into `main`.

## Status and findings

Investigation and CLI changes are complete in worktree `.tmp/agent3`, branch
`agent3-resize`. The old resize task is already complete; this is a new handoff.

Three different effects must not be confused:

- **Smooth history navigation** exists already, always enabled, with no CLI
  flag. `ScrollViewport::advance()` eases fractional row offsets with a 70 ms
  time constant. The native PTY loop calls `advanceViewport()` every frame.
  Use the mouse wheel or Shift+PageUp/PageDown in the primary screen with
  retained history. Application mouse reporting may capture the wheel;
  alternate-screen wheel input is not ordinary history navigation.
- **Smooth cursor movement** is opt-in: `--smooth-cursor`. Added on `main` in
  `e097b26569b53b7fb6fddb43246db4e9d9d5077c`. The argument is parsed in
  `app/main.cpp`, passed to `runPtyFrontend()`, and assigned to
  `CursorGlide::Config::enabled`. The native frame loop updates/applies the
  glide before cursor blink masking; SDL consumes its fractional offsets.
  This is wired up, not an ignored flag in native PTY mode. Default duration
  is 80 ms; jumps over 12 cells snap instead of gliding.
- **Smooth scrolling of newly arriving program output** is not implemented.
  Live output still advances by whole rows. There is no `--smooth-scroll`
  option. This remains separate future rendering work, not a CLI bug.

At the start, this worktree was at `5ccce80`, before the cursor implementation;
its executable would reject `--smooth-cursor`. I merged the existing `main`
changes into this branch to inspect and test the actual implementation. The
operator's executable/build path was not available, so a stale binary there is
plausible but not confirmed.

## Changes in this handoff

- `matterm --help` and `matterm -h` now return success and print options,
  defaults, usage and a cursor-animation example without SDL/font initialization
  or starting a shell. Previously `--help` was treated as an invalid option.
- Help and README distinguish smooth history, cursor glide and live output.
- Previously `--smooth-cursor` was silently ignored with `--showcase`, `--input`
  and in a build without PTY support. Those combinations now fail explicitly,
  rather than pretending to enable animation. The VT smoke-test mode is also
  rejected with this flag.
- CLI regression tests exercise help without a valid video driver or font,
  unsupported combinations, and rejection of the nonexistent scroll flag.
- A native dummy-video smoke test launches with `--smooth-cursor`, emits cursor
  movement and exits; it also checks that `--help` after the command separator
  reaches the child rather than being interpreted by matterm.

## How to launch

From an integrated checkout with the usual bundled font build:

```sh
cmake -S . -B build
cmake --build build -j4
./build/matterm --help
./build/matterm --smooth-cursor
./build/matterm --smooth-cursor -- /bin/bash -i
```

Use the exact newly built executable, not an older installed `matterm` on PATH.
The option belongs **before** `--`, not among the child's arguments. Use the
`matterm` application, not either standalone demo. `--showcase` is not native
PTY mode. With font bundling disabled, supply a font path first:

```sh
./build/matterm /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf --smooth-cursor
```

## Verification

Offline local build (SDL2 2.26.5, SDL2_ttf 2.20.1, system DejaVu Sans Mono):

```sh
cmake -S . -B build -DMATTERM_BUNDLE_FONT=OFF \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

- Native PTY build: **32/32 tests passed**, including existing cursor-glide CPU
  and renderer tests and the new CLI/native-launch tests.
- Separate `MATTERM_ENABLE_PTY=OFF` build: **23/23 tests passed**; manually
  checked that bare `--smooth-cursor` fails with the native-PTY requirement.
- Project C++ was run through clang-format; unrelated pre-existing formatting
  differences were excluded from the change. No third-party code was formatted.
- No physical-display visual check, browser build, or bundled-font download was
  performed. Tests verify startup and existing animation math/rendering, not
  perceived animation quality on the operator's display.

## Integration outcome

The CLI/help/documentation commit was applied to `main` as `c9ae518` without
reapplying cursor gliding or the old resize fix. Animated **live output** remains
separate future work. Pi Coding Agent draws its own cursor by default; enable
**Show hardware cursor** in Pi to let matterm animate it.
