# Smooth scrolling / cursor investigation

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

## Planner / integrator next steps

Cherry-pick the CLI/help/documentation commit onto `main` (which already has
`e097b26`); do not reapply the cursor implementation or the old resize fix.
Rebuild the executable the operator actually launches. If the intended request
was animated **live output**, plan that separately: this change does not add it.
If cursor animation still cannot be seen, collect the exact executable path,
launch command, build revision and whether the test was native PTY or a demo.
