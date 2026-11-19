# matterm

A reusable cell-grid renderer, working toward a terminal emulator. The standalone
`matterm-demo` demonstrates the library and will remain available independently
of the future terminal application. `matterm-terminal-demo` separately demonstrates
the CPU-only terminal model, including primary/alternate screens.
`TerminalMemory` supplies a borrowed row view through `RenderFrame` to
`SdlBackend`, which draws cell backgrounds and atlas glyphs in separate indexed
geometry batches. SDL2_ttf rasterizes glyphs on cache misses. This is not a
terminal yet.

Startup prints a matterm ASCII banner and opens a static welcome screen with
colored text and foreground/background gradients. Nothing streams automatically.
The demo retains up to 256 rows in reusable 16-row blocks. Controls:

- Mouse wheel or Up/Down scroll smoothly through history; Page Up/Down scroll by
  a page. Fractional wheel input is preserved for trackpads.
- Home jumps to the oldest retained page; End follows new output.
- S or Space starts/stops numbered output (one row every 100 ms).
- B appends a burst of 300 rows to exercise wraparound and eviction.
- R stops output and restores the welcome screen at the current window size.
- Tab cycles cursor shapes.
- A simple scrollbar on the right indicates the visible fraction and position;
  it is not clickable or draggable yet.
- The cursor blinks only at the live bottom. Reading history stays anchored as
  new rows arrive, until the viewed rows are evicted.
- Resize adjusts the visible row count and clips/fills stored row widths without
  reflow. Clipped text is not restored when widening again.
- Escape or closing the window exits. The title shows pause and scroll state.

## Terminal Model Demo

`matterm-terminal-demo` uses the model's direct API, not escape sequences:

- Type text (SDL UTF-8 input is converted to UTF-32 in the demo).
- Enter writes CR LF; arrows move the cursor; Home/End move within the row.
- F2 switches primary/alternate buffers. Returning restores primary text, cursor,
  colors, history, and scroll position. Re-entering alternate starts a fresh screen.
- F3 generates 40 lines; only the primary screen accumulates scrollback.
- F4 erases the active screen; F5 resets the entire demo.
- Wheel / Page Up/Down scroll; Ctrl+Home/End selects oldest history/live output.
- Escape exits. This is not a shell or a complete editor/terminal emulator.

## Native Terminal (Separate `matterm` Program)

Both existing demos keep their behavior. On Linux, `matterm` now starts an
interactive shell through a real PTY by default (`$SHELL -i`, or `/bin/sh -i`).
Use `-- COMMAND ARG...` to run a specific program without implicit shell quoting.
The native PTY starts before SDL workers and is resized after font metrics are known.

```sh
./build/matterm /path/to/font.ttf
./build/matterm /path/to/font.ttf -- /bin/bash --noprofile --norc -i
./build/matterm /path/to/font.ttf --showcase
```

In native terminal mode, Escape/function keys go to the program. Ctrl+C and Ctrl+D
are tty input, not GUI shortcuts. Close the window or press Ctrl+Shift+Q to quit.
Shift+Page Up/Down and Ctrl+Shift+Home/End navigate scrollback; ordinary navigation
keys go to the program. Ctrl+Shift+V pastes with control filtering and negotiated
bracketed paste. Selection/copy is not implemented; Ctrl+Shift+C is reserved.
After child exit the last frame remains visible with exit status in the title.

TERM defaults conservatively to `vt100`; `--term NAME` overrides it. Compatibility
is still incomplete: do not assume full xterm/Vim support or advertise richer
capabilities blindly. Use `--showcase` for the built-in VT stream, or `--input FILE`
for regular-file playback:

```sh
printf '\033[31mHello from VT\033[0m\r\n' > /tmp/matterm.vt
./build/matterm /path/to/font.ttf --input /tmp/matterm.vt
```

Wheel/Page Up/Down and Home/End navigate history; Space pauses file playback;
Escape exits. Without `--input`, it displays a parsed startup showcase. Supported
commands, bounds, callbacks, and limitations are documented in [docs/VT.md](docs/VT.md).
Native PTY/session and GUI integration are implemented, including parser replies,
keyboard input, resize, process cleanup, and bounded I/O. See [docs/PTY.md](docs/PTY.md) and
[ROADMAP.md](ROADMAP.md).

## Build and Run

Requires CMake 3.23+, a C++17 compiler, SDL2 2.0.18+, SDL2_ttf 2.0.18+, and a
TrueType font. A monospace font is recommended; no font is bundled.

On Debian/Ubuntu, install `libsdl2-dev`, `libsdl2-ttf-dev`, and optionally
`fonts-dejavu-core` for the example font below.

```sh
git submodule update --init
cmake -S . -B build
cmake --build build
./build/matterm-demo /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
./build/matterm-terminal-demo /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
./build/matterm /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
```

The project is named `matterm`; both demos remain separate clients of the library.
The separate `matterm` frontend does not replace either demo.

The demo requests an accelerated SDL renderer with VSync and prints the selected
backend. It no longer creates its own OpenGL context. The atlas has fixed capacity
and reports an error if full; it does not evict glyphs yet.

## Tests

Tests use the `mls-unit-test` submodule. Configure with `-DBUILD_TESTING=OFF` for
an application-only build. To enable font-dependent atlas/backend tests and a
headless rendering smoke tests for both demos, supply a font file:

```sh
cmake -S . -B build \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `TEST_FONT_FILE`, CTest runs the font-independent cell view, cell buffer,
terminal memory, terminal model, UTF-8 decoder, VT parser, scroll viewport, and
demo content tests. History tests cover
exact-capacity eviction, block and
ring boundaries, aliased input, resize, clear, and repeated wraps against a
reference queue. Viewport tests cover fractional offsets, animation convergence,
frame-rate independence, and anchoring during output/eviction. Backend tests verify
history views, cell colors, cursor shapes, fractional clipping, scrollbar geometry,
cell updates, and complete-frame clearing using framebuffer readback.
Model tests cover wrapping, controls, erasure, bounded history, resize, and screen
isolation. Smoke tests use SDL's dummy video driver and software renderer; the
terminal demo verifies rendered primary/alternate separation and restoration.
They do not validate accelerated GPU performance or visual quality on a real
display. VT tests compare whole streams with every split point, one-byte chunks,
and deterministic arbitrary bytes. The separate frontend smoke test renders
fragmented VT input and checks alternate-screen pixel restoration. Terminal
text-input regression checks also exercise ASCII and Swedish UTF-8 through the
demo's actual input helper. They can also be run directly:

```sh
SDL_VIDEODRIVER=dummy ./build/matterm-demo /path/to/font.ttf --smoke-test
SDL_VIDEODRIVER=dummy ./build/matterm-terminal-demo /path/to/font.ttf --smoke-test
SDL_VIDEODRIVER=dummy ./build/matterm /path/to/font.ttf --smoke-test
```

## Library and Applications

The build-tree CMake target `matterm::renderer` provides cell buffers, history,
viewport state, the atlas, and the SDL backend. A client can link it after adding
this project with `add_subdirectory`:

```cmake
add_subdirectory(path/to/matterm matterm-build)
target_link_libraries(my-app PRIVATE matterm::renderer)
```

`demo/main.cpp` shows SDL/SDL_ttf initialization, backend ownership, and rendering
borrowed cell memory. Demo-specific artwork and generated output live under
`demo/`, not in the reusable renderer. `demo/terminalmain.cpp` adds the CPU-only
`matterm::terminal` target and demonstrates the same rendering API with a terminal
model. Clients may use either level. `matterm::vt` is an optional incremental
UTF-8/VT subset feeding the same model, not a requirement for direct cell drawing. There is no
installed CMake package yet.

For a library-only build, disable the demo and tests:

```sh
cmake -S . -B build/lib-only -DMATTERM_BUILD_DEMO=OFF -DMATTERM_BUILD_APP=OFF -DBUILD_TESTING=OFF
cmake --build build/lib-only
```

The `matterm` application connects model/parser libraries to `matterm::session`
on Linux. `matterm::pty` and the session adapter are separately linkable; disable
native support with `-DMATTERM_ENABLE_PTY=OFF` for portable builds. Without native
support, the app defaults to the VT showcase. A standalone memory viewer could also
feed cell views into it. Currently views borrow memory in the same process;
attaching to shared memory or another process will need an explicit adapter and
synchronization protocol, not raw cross-process pointers.

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

This is row storage; terminal behavior is provided by the separate model below.
There is no text cluster pool yet.

## Terminal Model Library

`matterm::terminal` provides `TerminalModel`; `matterm::utf8` independently
provides `Utf8Decoder`, without SDL dependencies or a required GUI loop. Its UTF-32 `write()` handles CR, LF, backspace, and 8-column tabs;
printable characters wrap only when another character follows the last column.
LF preserves the column, so use `U"\r\n"` to start a new line at column zero.

Cursor placement, colors, inclusive line/display erasure, and primary/alternate
screen switching are direct API operations. Primary capacity is screen height
plus the requested scrollback limit. Alternate capacity is exactly screen height.
Each buffer has its own cursor, colors, pending wrap, and scroll viewport.

`resize()` requires positive dimensions. Width clips/pads without reflow; height
shrink keeps bottom rows (primary top rows become history), while growth adds
blank rows below. Both buffers resize together with strong exception safety.
`frame(cellHeight)` borrows the active cells for rendering; blink timing remains
with the caller. Screen mutations and subsequent frames invalidate borrowed views.

`Utf8Decoder` incrementally converts UTF-8 byte chunks, preserves split characters,
and replaces malformed input with U+FFFD. `finish()` flushes a truncated prefix.
The terminal demo uses it instead of platform-dependent SDL/iconv conversions.
It is Unicode decoding only, not a VT parser.

Use the separate `VtParser` for ANSI/VT byte streams, not UTF-32 `write()`.
The supported VT subset is limited; the Linux PTY is a separate optional adapter.
Scrolling margins, origin/autowrap/insert modes, character/line insertion and
deletion, region scrolling, and reverse index are available through the model and
VT layer. Full-screen scrolls retain primary history; partial-region edits do not.
Width/height resize resets margins and origin mode to the full screen.
Custom tab stops, bold/italic, underline/strike, faint/blinking text, and cursor
shape/blink controls are implemented. FChar remains 12 bytes. Wide characters,
combining clusters, and full DEC mode rules are not implemented; currently one
code point occupies one cell. Palettes retain default/indexed/RGB identity in the same 12-byte cell. The native
terminal uses a Tango/GNOME-inspired dark default. Set RenderFrame.palette to a
caller-owned Palette to recolor existing indexed/default cells; literal RGB artwork
stays unchanged. See docs/COLORS.md for exact offsets/flags and compatibility.
Persistent settings and reported scroll-boundary issues remain tracked in ROADMAP.md.

## Rendering Boundary

`RenderFrame` borrows the cell view and carries `CursorState`. `RenderBackend`
exposes `render(frame)` and `present()`. `SdlBackend` owns the SDL renderer and
atlas; the GUI loop owns the backend and keeps its window alive until after the
backend is destroyed. Frame views are not retained after rendering.

The SDL backend renders at the drawable's pixel resolution and reports cell
pixel metrics and how many whole cells fit. It redraws the complete frame,
including clearing areas outside the grid. Cursor blinking belongs to the app;
the backend only draws the supplied state, without changing stored cells.

`ScrollViewport` separates the desired and displayed offsets in fractional rows,
using frame-rate-independent easing. `RenderFrame` supplies a negative pixel
offset, an extra partially visible row, text clip height, and scrollbar proportions.
The SDL backend clips text/cursor drawing and reserves a 12-pixel scrollbar strip
when requested; `columns(true)` accounts for that strip. The demo uses a whole-cell
text height, leaving any remainder pixels below the text clear. Live output still
advances by whole rows; smooth animation currently applies to user navigation.

The alternate screen is available to library clients as well as the terminal
demo. The renderer has no terminal mode logic: the model supplies the active view
and hides the alternate screen's scrollbar. The demo keeps equal column counts
in both modes so hiding the sidebar does not itself resize the primary screen.
Full-screen/editor compatibility still needs broader VT support; having a PTY and
alternate screen alone does not imply complete Vim compatibility.

## Design and Roadmap

See [ROADMAP.md](ROADMAP.md) for the implementation checklist, parser/PTY stages,
and deferred terminal-output, Emscripten, remote-browser, and simulated-browser
client ideas. Source layers are `src/core/`
(cells/storage), `src/render/` (graphics), and `src/terminal/` with `vt/` for stream
parsing. Both existing demos remain independent of the future `matterm` app.

See [DESIGN.md](DESIGN.md) for ownership, cell layout, scrollback storage, backend
boundaries, and planned milestones. The current atlas is deliberately SDL-specific;
a future OpenGL implementation can reveal which parts are worth sharing.
