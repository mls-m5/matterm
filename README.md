# 2026 Terminal Renderer

An SDL2 glyph-atlas demo, working toward a terminal emulator. It rasterizes glyphs
with SDL2_ttf, caches them in one texture, and draws colored text in one indexed
geometry batch. Escape or closing the window exits. This is not a terminal yet.

## Build and Run

Requires CMake 3.23+, a C++17 compiler, SDL2 2.0.18+, SDL2_ttf 2.0.18+, and a
TrueType font. A monospace font is recommended; no font is bundled.

On Debian/Ubuntu, install `libsdl2-dev`, `libsdl2-ttf-dev`, and optionally
`fonts-dejavu-core` for the example font below.

```sh
git submodule update --init
cmake -S . -B build
cmake --build build
./build/2026-terminal-renderer /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
```

The demo requests an accelerated SDL renderer with VSync and prints the selected
backend. It no longer creates its own OpenGL context. The atlas has fixed capacity
and reports an error if full; it does not evict glyphs yet.

## Tests

Tests use the `mls-unit-test` submodule. Configure with `-DBUILD_TESTING=OFF` for
an application-only build. To enable the font-dependent atlas tests and a headless
rendering smoke test, supply a font file:

```sh
cmake -S . -B build \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `TEST_FONT_FILE`, CTest runs the font-independent cell view and cell
buffer tests.
The smoke test uses SDL's dummy video driver and software renderer and verifies
that glyph pixels reach the framebuffer. It does not validate accelerated GPU
performance or visual quality on a real display. It can also be run directly:

```sh
SDL_VIDEODRIVER=dummy ./build/2026-terminal-renderer /path/to/font.ttf --smoke-test
```

## CPU Cell Buffer

`CellBuffer` provides cell-coordinate text placement, clearing, checked cell
access, and resize preserving the top-left intersection. Draws clip without
wrapping or interpreting terminal control characters. Input can be formatted
`FStringView` cells or UTF-32 text with foreground/background colors; it does not
decode UTF-8 or handle wide characters yet.

`CellGridView` borrows row descriptors, allowing non-adjacent rows in future
scrollback storage. A buffer's views reflect cell edits and remain valid until
resize or destruction. The demo is not connected to the cell buffer yet.

## Design and Roadmap

See [DESIGN.md](DESIGN.md) for ownership, cell layout, scrollback storage, backend
boundaries, and planned milestones. The current atlas is deliberately SDL-specific;
a future OpenGL implementation can reveal which parts are worth sharing.
