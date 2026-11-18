# 2026 Terminal Renderer

An interactive scrollback demo, working toward a terminal emulator.
`TerminalMemory` supplies a borrowed row view through `RenderFrame` to
`SdlBackend`, which draws cell backgrounds and atlas glyphs in separate indexed
geometry batches. SDL2_ttf rasterizes glyphs on cache misses. This is not a
terminal yet.

The demo retains 256 rows in reusable 16-row blocks, prefills beyond capacity,
and appends a numbered row every 100 ms. Controls:

- Mouse wheel or Up/Down scroll through history; Page Up/Down scroll by a page.
- Home jumps to the oldest retained page; End follows new output.
- Space pauses/resumes output; Tab cycles cursor shapes.
- The cursor blinks only at the live bottom. Reading history stays anchored as
  new rows arrive, until the viewed rows are evicted.
- Resize adjusts the visible row count and clips/fills stored row widths without
  reflow. Clipped text is not restored when widening again.
- Escape or closing the window exits. The title shows pause and scroll state.

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
an application-only build. To enable font-dependent atlas/backend tests and a
headless rendering smoke test, supply a font file:

```sh
cmake -S . -B build \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `TEST_FONT_FILE`, CTest runs the font-independent cell view, cell buffer,
and terminal memory tests. History tests cover exact-capacity eviction, block and
ring boundaries, aliased input, resize, clear, and repeated wraps against a
reference queue. Backend tests verify history views, cell colors, cursor shapes,
cell updates, and complete-frame clearing using framebuffer readback.
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

`CellGridView` borrows row descriptors and supports non-adjacent rows. A
CellBuffer's views reflect cell edits and remain valid until resize or destruction.
CellBuffer remains available for scratch surfaces; the demo now reads terminal
memory directly instead of copying the visible cells into a separate buffer.

## Terminal Memory

`TerminalMemory` stores fixed-width rows in a bounded, block-backed ring.
Capacity includes all retained rows, both visible and historical. Storage is
allocated upfront; append reuses a row slot and never shifts surviving cells.
Each block stores contiguous cells. A partial last block allows exact capacities
that are not multiples of the block size.

`append()` clips/pads a row, `row(0)` reads the oldest retained row, and
`view(first, count)` builds only visible row descriptors without copying cells.
Borrowed views expire on the next `view()` call, mutation, or destruction.
`clear()` reuses storage. `resizeColumns()` explicitly reallocates and copies
retained cells, without text reflow; ordinary appends do not allocate row storage.
The demo owns scroll position separately from the stored history.

This is row storage, not yet a terminal screen model, parser, or text cluster pool.

## Rendering Boundary

`RenderFrame` borrows the cell view and carries `CursorState`. `RenderBackend`
exposes `render(frame)` and `present()`. `SdlBackend` owns the SDL renderer and
atlas; the GUI loop owns the backend and keeps its window alive until after the
backend is destroyed. Frame views are not retained after rendering.

The SDL backend renders at the drawable's pixel resolution and reports cell
pixel metrics and how many whole cells fit. It redraws the complete frame,
including clearing areas outside the grid. Cursor blinking belongs to the app;
the backend only draws the supplied state, without changing stored cells.

## Design and Roadmap

See [DESIGN.md](DESIGN.md) for ownership, cell layout, scrollback storage, backend
boundaries, and planned milestones. The current atlas is deliberately SDL-specific;
a future OpenGL implementation can reveal which parts are worth sharing.
