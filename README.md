# matterm ♥ matmux

A terminal emulator and multiplexer that runs both as a native app and in the browser, with
terminals that keep running in a daemon, and as `matmux` in any terminal (a small tmux). Built in
C++ on a reusable cell-grid renderer and a small terminal model.

**[Try it online](https://mls-m5.github.io/matterm/)**

## What is in here

- `matterm`: the native terminal (SDL2, a real PTY), Linux, with tabs and split panes
  (Ctrl+Shift+T, Ctrl+Shift+E/O; see [docs/OVERVIEW.md](docs/OVERVIEW.md)).
- `matmux`: the same terminals in the terminal you are in (also over ssh from a phone): start one, leave it
  running, come back to it; one static file, no window libraries (see "Keep terminals running" below).
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

## Getting started: a terminal on the laptop, shown in a browser

1. **Start the app**: `./build/matterm`. The daemon starts by itself (it is the default); the
   terminals live in it, so closing the window does not end them. `--no-daemon` runs them in the
   window instead. If the daemon cannot start, the first terminal says so.
2. **Share a terminal** (only shared ones are ever visible to a browser): click *Share* at the end
   of the tab strip, or Ctrl+Shift+P and *Share this terminal* / *Share this tab*. A shared pane
   has a small red square in its corner. *Rename terminal...* gives it a name that shows in the
   lists.
3. **Run the web server with exposure**: `matterm-server --expose-local-sessions` (it must run as
   the same user as the daemon; see [docs/SERVER.md](docs/SERVER.md)). Log in from the browser.
4. **Take it over in the browser**: in the web page open the palette (Ctrl+B `:`, or the button at
   the end of the strip) and choose **Local terminals...**, then the terminal. The laptop pane
   pauses with "Press Enter to take it back"; Enter on the laptop takes it back, and the browser
   is told.
5. **Another laptop window** can show any terminal of the daemon (also unshared ones, it is your
   own user): palette *Attach terminal...*.
6. Ctrl+Shift+H lists every keyboard shortcut.

## Settings

Run `matterm --settings` or `matmux --settings` in a terminal, or choose **Settings**
in the native command palette (Ctrl+Shift+P) to open a new local tab. Even with
`--daemon-host`, this edits the client's settings, not a file on the other computer.

Type to search; Tab switches sections/controls, arrows select, and Enter edits or
applies. Changes are saved immediately and the window follows within about one
second. Esc cancels the current edit, not earlier saves; Ctrl+Q exits. Themes,
per-computer overrides, computer-name placement (center/left/right), tab activity,
window frame, scrollback, copy behaviour and close confirmation are available.
Frame/scrollback changes apply to new windows/terminals. The
`cellVerticalAlignment` setting is `"top"` by default; choose `"bottom"` to
place the spare pixels above each terminal pane instead of below it. Either
way, the outermost cell backgrounds extend into those spare pixels without
covering terminal text or the resize border. By default,
`copyJoinFullTuiRows` is `true`: when copying from a full-screen TUI, trailing
spaces are removed and a line reaching the right edge (including up to two
trailing spaces) is joined with the next nonblank line. Set it to `false` in
Settings to keep those line breaks; rectangular copies always keep their rows.
The terminal cannot always distinguish a TUI's artificial break from a
purposeful newline at the right edge.

The file is `$XDG_CONFIG_HOME/matterm/settings.json` (otherwise
`~/.config/matterm/settings.json`): strict JSON, always formatted with defaults
included. Unknown members are retained. External changes are re-read before
applying a field; conflicts or broken files are reported instead of overwritten.
Keys, values and theme rules: [docs/TABS_PLAN.md, section 17](docs/TABS_PLAN.md#17-settings-and-themes-agent2-decided-by-the-owner-2026-11-23).

## The window frame

The native window draws its own frame: the tab strip is the title bar (drag the empty part, minimise,
maximise and close at the right) and the edges resize. `matterm --system-frame` uses the desktop's frame
instead; where ours cannot work, the desktop's is used and a note says so. `matterm --window-test`
prints which window calls work on this desktop. Details: [docs/TABS_PLAN.md](docs/TABS_PLAN.md)
section 16.

## Keep terminals running: the local daemon

`matterm-daemon` (built with `-DMATTERM_BUILD_SERVER=ON`, Linux) keeps terminals alive on a
private Unix socket, so they survive closing the window or a crash of the GUI.

```sh
./build/matterm --daemon            # starts the daemon if it is not running (default; --no-daemon runs the programs in the window)
./build/matterm --daemon --daemon-socket /tmp/dev.sock   # a separate daemon, for development
./build/matterm --daemon-host user@host                  # the daemon of another computer, through an ssh tunnel (docs/REMOTE_DAEMON.md)
```

- The socket is `$XDG_RUNTIME_DIR/matterm/daemon.sock` (directory 0700, socket 0600, only your
  user can connect); the daemon runs your `$SHELL` and ends itself after 60 s with nothing in it.
- Ctrl+Shift+H (browser: Ctrl+B ?) lists all keyboard shortcuts; in the browser, waiting half a
  second after Ctrl+B shows which keys can follow.
- **Attach terminal...** lists the daemon's terminals (also ones left over after a restart) and opens
  one in a new tab; **Detach terminal (keep running)** and **Move terminal to new tab** are there too.
- Ctrl+Shift+P opens the command palette: **Daemon status**, **Stop daemon** (asks, and says how
  many programs end), **Quit and keep running** (closes the window, the programs stay).
  Closing a pane or quitting normally ends its terminal; SIGTERM/SIGHUP and *Quit and keep
  running* leave them. Start `matterm --daemon` again to get a new window; open terminals are
  still listed by the daemon: *Attach terminal...* opens one again.
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
- **Without a window, from a phone**: `ssh -t you@computer matterm --attach-tty` lists the terminals
  of the daemon, takes the one you choose over and shows it full screen in that terminal (`--new` starts a terminal
  first, a small tmux); Ctrl+B d leaves it running, Ctrl+B l goes back to the list. `matmux` is the same without the
  window libraries, one static file (`--daemon-install` copies it to the other computer): `matmux` shows the list (New
  terminal chosen; when nothing runs it starts a terminal at once), `matmux --attach` lists with the first terminal chosen (`x` ends a terminal after a question), `matmux --new --name work -- htop`. matmux passes the program's output through to your terminal, so its own scrollback, selection and touch scrolling work (`--rendered` draws it cell by cell instead). See
  [docs/REMOTE_DAEMON.md](docs/REMOTE_DAEMON.md) section d.
- Details: [docs/tasks/daemon.md](docs/tasks/daemon.md), [docs/PROTOCOL.md](docs/PROTOCOL.md)
  section 12, [docs/TABS_PLAN.md](docs/TABS_PLAN.md) sections 10-11.

## More

- [docs/COMPLETION.md](docs/COMPLETION.md): tab completion for the options (bash, zsh, fish).
- [docs/OVERVIEW.md](docs/OVERVIEW.md): the full guide (controls, options, tests,
  library use, rendering and terminal model).
- [ROADMAP.md](ROADMAP.md), [DESIGN.md](DESIGN.md), [docs/](docs/): plans, design and
  reference.
- [Agents.md](Agents.md): how work is organized.
