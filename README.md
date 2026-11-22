# matterm

A terminal emulator in C++ that runs both as a native app and in the browser, on a
reusable cell-grid renderer and a small terminal model.

**[Try it online](https://mls-m5.github.io/matterm/)**

## What is in here

- `matterm`: the native terminal (SDL2, a real PTY), Linux, with tabs and split panes
  (Ctrl+Shift+T, Ctrl+Shift+E/O; see [docs/OVERVIEW.md](docs/OVERVIEW.md)).
- `matterm-server` and the web client: the same terminal in a browser, with a
  password-protected server that owns the shell (see
  [docs/SERVER.md](docs/SERVER.md), [web/README.md](web/README.md)).
- `matterm-demo`: the showcase behind "Try it online".
- `chatd` and `mattrix-chat`: a small live chat (one room, messages kept only in memory, your draft is shown to the others as you type) and its terminal client; see [docs/CHAT.md](docs/CHAT.md) for the design and for running it locally.
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

## Keep terminals running: the local daemon

`matterm-daemon` (built with `-DMATTERM_BUILD_SERVER=ON`, Linux) keeps terminals alive on a
private Unix socket, so they survive closing the window or a crash of the GUI.

```sh
./build/matterm --daemon            # starts the daemon if it is not running (default; --no-daemon runs the programs in the window)
./build/matterm --daemon --daemon-socket /tmp/dev.sock   # a separate daemon, for development
```

- The socket is `$XDG_RUNTIME_DIR/matterm/daemon.sock` (directory 0700, socket 0600, only your
  user can connect); the daemon runs your `$SHELL` and ends itself after 60 s with nothing in it.
- Ctrl+Shift+P opens the command palette: **Daemon status**, **Stop daemon** (asks, and says how
  many programs end), **Quit and keep running** (closes the window, the programs stay).
  Closing a pane or quitting normally ends its terminal; SIGTERM/SIGHUP and *Quit and keep
  running* leave them. Start `matterm --daemon` again to get a new window; open terminals are
  still listed by the daemon (re-attaching old ones from the GUI is not built yet).
- **Sharing**: with `--daemon` the end of the tab strip has a *Share* button (grey: nothing in
  the tab is shared; red with a count: that many panes are), and the palette has *Share this
  terminal*, *Share this tab* and the matching *Stop sharing* entries. A shared pane has a
  small red square in its corner. Only shared terminals are ever offered to a browser.
- If another window takes a terminal over, the pane keeps its screen, pauses input and says
  "This terminal is controlled from another window. Press Enter to take it back".
- **From a browser**: `matterm-server --expose-local-sessions [--daemon-socket PATH]` offers the
  shared terminals to logged-in browsers. In the web page the palette gets **Local terminals...**
  (only when the server offers them); choosing one opens a tab attached to it, and the laptop's
  window shows the take-over notice. Enter in the browser pane takes it back after the laptop
  took it again. `web/tools/e2e-local.js` checks the whole path with a real daemon.
- Details: [docs/tasks/daemon.md](docs/tasks/daemon.md), [docs/PROTOCOL.md](docs/PROTOCOL.md)
  section 12, [docs/TABS_PLAN.md](docs/TABS_PLAN.md) sections 10-11.

## More

- [docs/OVERVIEW.md](docs/OVERVIEW.md): the full guide (controls, options, tests,
  library use, rendering and terminal model).
- [ROADMAP.md](ROADMAP.md), [DESIGN.md](DESIGN.md), [docs/](docs/): plans, design and
  reference.
- [Agents.md](Agents.md): how work is organized.
