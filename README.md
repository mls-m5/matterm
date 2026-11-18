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

## Design and roadmap

See [DESIGN.md](DESIGN.md) for the agreed architecture, ownership, cell layout,
scrollback storage, rendering backend interface, and implementation milestones.
The design describes planned work; the application still only clears a window.
