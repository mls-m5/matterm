# matterm

A terminal emulator in C++ that runs both as a native app and in the browser, on a
reusable cell-grid renderer and a small terminal model.

**[Try it online](https://mls-m5.github.io/matterm/)**

## What is in here

- `matterm`: the native terminal (SDL2, a real PTY), Linux.
- `matterm-server` and the web client: the same terminal in a browser, with a
  password-protected server that owns the shell (see
  [docs/SERVER.md](docs/SERVER.md), [web/README.md](web/README.md)).
- `matterm-demo`: the showcase behind "Try it online".
- `mattrix`: Mattrix OS, the pretend operating system of the demo, as a program for any terminal (no SDL; also runs inside `matterm -- ./mattrix`).
- `matterm-render-demo`, `matterm-lib-demo`: small examples of using the libraries
  (`matterm::renderer`, `matterm::terminal`, `matterm::vt`).

## Build and run

Needs CMake 3.23+, a C++17 compiler, SDL2 and SDL2_ttf (on Debian/Ubuntu:
`libsdl2-dev libsdl2-ttf-dev`). The font is downloaded and bundled.

```sh
git submodule update --init
cmake -S . -B build
cmake --build build
./build/matterm
```

The browser build and the server are described in [web/README.md](web/README.md) and
[docs/SERVER.md](docs/SERVER.md) (`-DMATTERM_BUILD_SERVER=ON`).

## More

- [docs/OVERVIEW.md](docs/OVERVIEW.md): the full guide (controls, options, tests,
  library use, rendering and terminal model).
- [ROADMAP.md](ROADMAP.md), [DESIGN.md](DESIGN.md), [docs/](docs/): plans, design and
  reference.
- [Agents.md](Agents.md): how work is organized.
