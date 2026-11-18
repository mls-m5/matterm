# 2026 Terminal Renderer

A minimal SDL2 + OpenGL application. It opens a resizable OpenGL window; press
Escape or close the window to exit.

## Build

Requires CMake 3.23+, a C++17 compiler, SDL2 development files, and OpenGL
development files.

```sh
cmake -S . -B build
cmake --build build
./build/2026-terminal-renderer
```
