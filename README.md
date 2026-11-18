# 2026 Terminal Renderer

A minimal SDL2 + OpenGL application. It opens a resizable OpenGL window; press
Escape or close the window to exit.

## Build

Requires CMake 3.23+, a C++17 compiler, SDL2 development files, and OpenGL
development files.

Tests use the `mls-unit-test` submodule. Initialize it before building, or
configure with `-DBUILD_TESTING=OFF` for an application-only build.

```sh
git submodule update --init
cmake -S . -B build
cmake --build build
./build/2026-terminal-renderer
ctest --test-dir build --output-on-failure
```

## Terminal renderer roadmap

`src/matrixrenderer.h` defines the first-pass cell format and a read-only view
of the visible grid. The SDL app still only clears the window; `drawFrame` is
an interface sketch, not an implemented OpenGL backend yet.

1. **Build the CPU grid.** Own a contiguous, row-major array of cells. Keep
   text placement, UTF-8 decoding, wrapping, scrolling, and ANSI/VT state out
   of the renderer. Start with ASCII for the first visual milestone, then add
   wide-cell continuations and combining characters in the terminal model.
2. **Draw the first grid.** Use a bitmap glyph atlas and instanced quads in
   OpenGL 3.3. Start with one background pass and one glyph pass, not a draw
   call per cell. Upload the visible grid each frame as the initial baseline;
   SDL continues to own the buffer swap.
3. **Measure before optimizing.** Benchmark a mostly static prompt, sustained
   scrolling, and full-screen redraws. Track CPU frame time, GPU draw time,
   bytes uploaded, and draw calls with VSync disabled for the benchmark.
   If upload cost matters, track changed rows and update those instance ranges.
   Still redraw the frame on swap; dirty uploads do not imply preserved
   back-buffer contents.
