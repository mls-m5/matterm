# Task: fakeos, a pretend Unix system with ASCII animations (the static demo)

Plan agreed with the owner; implementation is split into the milestones below.
Assignment for an independent agent in its own worktree and branch (base: `main`).
Read first: Agents.md, docs/PARALLEL_WORK.md, docs/SIMULATOR.md and
`src/transport/scriptedlink.*` (the `Program` interface and the `Simulator`),
`src/transport/terminallink.h`, docs/WEB_TERMINAL.md (layering), web/README.md
(`?demo`, the static client files) and `src/platform/posix/ptyprocess.*` only to
see which POSIX calls the native host will need.

## Decisions (agreed with the owner)

- **No server for the demo.** The demo is the static browser build: `ScriptedLink`
  runs the pretend system in the page. It can be hosted by any static file server
  (no matterm-server, no WebSocket, no login). This replaces the earlier idea of a
  demo server; nothing in `src/server/` changes.
- **A pretend system, not a shell.** An in-memory Unix-like world: a virtual file
  system, a small shell and commands, and full-screen ASCII animations started
  like any command (`fire`, `gradient`, later `crumble`). It is not connected to
  the real system at all, so `rm -rf /` only removes virtual files, and a new
  session starts from the same pristine state.
- **One core, several hosts.** The core is plain C++17 that turns bytes in into
  bytes out. The same code runs (1) in the browser through `ScriptedLink`, (2) as
  a native executable `matterm-fakeos` in any terminal, and later (3) inside the
  native matterm app. Running the demo in a stock terminal (gnome-terminal, xterm,
  a Linux console that supports the used sequences) is a goal the design must not
  block, even though only matterm itself is tested at first.
- **First delivery includes an animation** (`fire`, plus `gradient`), so the time
  and frame design is proven from the start.

## Goal

`fakeos` as a library plus two thin hosts, so that opening the static page, or
running `matterm-fakeos` in a terminal, gives a prompt in a believable pretend
Unix where files can be browsed and edited, pipes work, and `fire` shows an
animated fire until `q`.

## Architecture

```
bytes in --+
resize  ---+--> fakeos::System --> bytes out (plain VT)
tick(now) -+        |
                    +--> nextDeadline() for the host's loop
```

- `src/fakeos/` (new library `matterm::fakeos`): no I/O of any kind. It must not
  include `<filesystem>`, `<fstream>`, `<unistd.h>`, SDL or Emscripten headers,
  start threads, or read a clock: time arrives through `tick(now)`, randomness
  from a seeded PRNG in the config (so tests and screenshots are deterministic).
  A test or CMake check enforces the include rule.
- `System` API (sketch): `Config {columns, rows, seed, user, hostname}`,
  `Reaction start()`, `Reaction input(bytes)`, `Reaction tick(now)`,
  `void resize(columns, rows)`, `std::optional<Time> nextDeadline() const`;
  `Reaction` is `{output, exitCode}` as in `ScriptedLink`.
- **`ScriptedLink::Program` gains `tick(now)` and `nextDeadline()`** (default: no
  output, no deadline, so `Simulator` and its tests stay valid). `ScriptedLink`
  calls the program's `tick` from its own `tick` and merges the deadline. This is
  the only change outside the new directories besides build files.
- **Layers inside fakeos:**
  - `VirtualFs`: tree of directories and files in memory, modes and owners, size
    and count limits (a runaway `yes > f` must hit a limit, not memory), seeded
    with `/etc/motd`, `/etc/passwd` style files, `/home/guest` with a few
    examples, `/usr/share/...` with text for `cat` and `grep`.
  - `Shell`: line editor (the `Simulator`'s plus history on Up/Down and Tab
    completion of commands and paths), tokenizer with quotes and escapes,
    variables (`$HOME`, `$?`, `export`), `cwd`, pipes `|`, redirects `>` `>>` `<`,
    `;` and `&&`/`||`, exit status, `Ctrl-C`/`Ctrl-D`.
  - `Commands`: pure functions `(Context&, args, stdin) -> output, status`:
    `ls` (`-l -a`), `cd`, `pwd`, `cat`, `echo`, `mkdir`, `touch`, `rm`, `cp`,
    `mv`, `head`, `tail`, `wc`, `grep`, `sort`, `uniq`, `uname`, `whoami`, `id`,
    `hostname`, `date` (from the injected clock), `env`, `ps` (a few fake
    processes), `uptime`, `clear`, `reset` (restores the pristine world),
    `help`/`man`, `colors`, `exit`. Unknown commands print the shell's usual
    "command not found". Real-looking errors and exit codes matter more than
    breadth.
  - `App`: interface for full-screen programs, `onInput`, `tick`, `resize`,
    `render`. The shell runs one in the foreground; it uses the alternate screen
    and hides the cursor, and always restores both on exit (also on `Ctrl-C` and
    `q`).
  - `AsciiCanvas`: a cell buffer with colors that emits only the cells that
    changed since the previous frame (cursor addressing plus SGR), so animations
    do not flicker and stay light on a stream. Frame rate at most about 30 per
    second; a frame is skipped when nothing changed.
  - Animations: `fire` (the cellular fire algorithm with a palette, truecolor
    when available and a 256-color fallback), `gradient` (smooth truecolor
    gradients and a moving hue band). Later: `crumble`, see milestone 3.
- **Capability handling:** the core takes a `Capabilities {truecolor, colors256,
  alternateScreen, unicode}` in its config. The matterm hosts say yes to all of
  them. The native host decides from `COLORTERM`, `TERM` and the locale and the
  animations degrade (256 colors, ASCII ramps ` .:-=+*#%@`) instead of failing.
  Only standard ECMA-48/xterm sequences are emitted, nothing matterm-specific.
- **Native host `matterm-fakeos`** (`app/fakeos/` or `src/platform/posix/`): refuses
  to run unless stdin and stdout are terminals, puts the tty in raw mode, loops
  on `poll` (timeout from `nextDeadline()`), reads stdin into `input()`, writes
  the output with partial-write and `EAGAIN` handling, maps `SIGWINCH` to
  `resize()` through a self-pipe, and restores the terminal on every way out
  (normal exit, `SIGINT`/`SIGTERM`/`SIGHUP`, exceptions). It opens no files except
  the tty and spawns nothing.
- **Browser host:** `web/main.cpp`'s no-server branch creates
  `ScriptedLink` with a `fakeos::System` program instead of the plain `Simulator`
  (small change in that block only; the integrator owns the rest). `frame()`'s
  idle throttling must stay awake while an animation runs: animation frames change
  the frame signature, which already extends the active window, so no new rule is
  expected; verify it.
- **Static hosting:** the page must work from any static server with only files:
  `index.html`, `index.js/.wasm/.data`, `login.js`, `style.css`. Add a way to start
  in demo mode without a query string (for example a second entry page
  `demo.html` that loads the same files with demo mode on, or `window.mattermDemo`
  set before `login.js` runs) and document the copy-this-directory deployment in
  web/README.md. No CSP is assumed (static hosts often cannot set headers); the
  page needs no inline script anyway.

## Milestones (each ends with tests and a commit series)

1. **Core and first animation.** `src/fakeos/`: `VirtualFs`, `Shell`, the file and
   text commands, `AsciiCanvas`, the `App` interface, `fire` and `gradient`,
   the `ScriptedLink::Program` tick extension, the browser demo using it, tests.
2. **Native host.** `matterm-fakeos` and its tests (a PTY-driven test using the
   existing `PtyProcess` helpers: start it, type commands, check the output,
   resize, `fire` and `q`, and that the terminal is restored; skipped if no PTY).
3. **More animations and commands.** `crumble` (an ASCII text or box breaking
   into blocks that fall, bounce and settle, simple deterministic physics),
   `matrix`-style rain if wanted, `top`, a tiny pager (`less`) that exercises the
   alternate screen and mouse reporting, `tree`, `cal`, `fortune`.
4. **Static packaging.** The demo entry page, README section, a Playwright check
   that the page runs from a plain static HTTP server, with no matterm-server,
   and that `fire` animates (two screenshots differ) and `q` returns to the
   prompt.

## Out of scope

Real processes, files or network from the demo (never, by design), scripting
language features (functions, loops, `if` in the shell), job control, a package
manager, recording and playback (see docs/MULTI_TERMINAL.md), changing the
server, protocol, `TerminalLink`, `ClientSession` or `src/terminal/`, and
settings UI.

## Owned and shared paths

Owned: `src/fakeos/`, `app/fakeos/` (or the chosen native host path),
`test/fakeos_*`, `docs/FAKEOS.md` (user documentation: commands, animations,
limits, how to run natively and hosted statically), the demo entry page.
Shared, edit minimally and report: `src/transport/scriptedlink.*` (the
`Program::tick` extension), root and `test/CMakeLists.txt`, `web/CMakeLists.txt`,
`web/main.cpp` (the no-server block only), `web/README.md`, `web/tools/*`,
`docs/SIMULATOR.md` (mention fakeos).

## Acceptance

- Unit tests with a fake clock and fixed seed: VirtualFs operations and limits,
  tokenizer and pipeline cases, every command's output and exit status, history
  and completion, `rm -rf /` leaving the program functional, `reset`, canvas
  diffing (an unchanged frame emits nothing), `fire` frames deterministic for a
  seed, `q` and `Ctrl-C` restoring the screen mode and cursor. Feed output
  through `VtParser` into a `TerminalModel` and assert screen text, as the
  `ScriptedLink` tests do.
- The include-rule check passes (no filesystem, process or clock access in
  `src/fakeos/`).
- The full ctest suite passes in the default and `-DMATTERM_BUILD_SERVER=ON`
  configurations; the native executable runs under a PTY test; the web build
  and Playwright check run when the tools exist in the sandbox, and the report
  says plainly what could not be run. clang-format applied, no build products
  committed, `.matterm.files` updated locally.

## Decisions and open questions

Decided by the owner:

- **No persistence**: a fresh virtual world every page load; nothing is saved
  (no `localStorage` opt-in for now).
- **Mouse input in the web demo**: the demo may show clickable text (for example
  a menu or command links). Do it through the normal terminal mouse reporting:
  the program turns on DECSET 1000/1006 and receives the same SGR mouse reports
  a real application would, so the core needs no special click path. The clicked
  cell is mapped to the text that was drawn there. Not needed for milestone 1,
  but keep the `Program` interface able to receive mouse reports (they arrive as
  input bytes).
- **Naming and the native demo window** (decided in principle; do the renames in
  the native-host milestone, as a separate commit):
  - the new showcase becomes `matterm-demo`: an SDL window that runs the fakeos
    core through the matterm model/renderer (the "native app" host);
  - the current renderer-only `matterm-demo` is renamed `matterm-lib-demo`;
  - `matterm-terminal-demo` (the model's direct API, no escape sequences) stays as
    it is. Agents.md, README.md, docs/FONTS.md and CMake mention the old names and
    must be updated together with the rename (the owner authorizes this change to
    the earlier "keep matterm-demo standalone" rule).
  - `matterm-fakeos` (the raw-tty program that runs inside any terminal) stays a
    separate target.
- **Intro animation**: opening the demo (web and native window) plays a short intro
  animation (title/logo reveal in the ASCII style of the other animations) before
  the shell prompt appears. Any key or click skips it; `?nointro` (web) and
  `--no-intro` (native) turn it off; it is part of milestone 1 and uses the same
  animation core, with seeded randomness so tests are deterministic.
- **Embedding in a homepage**: the static demo must be usable inside another
  page, as an `<iframe src=".../demo.html">` (simplest, isolated) and, if cheap, a
  small script that mounts it into a `<div>`. Requirements: the demo page is
  static and hosted *separately* from `matterm-server` (never frame the real
  terminal; the server keeps refusing framing), carries no cookies or secrets,
  is responsive to the iframe size (the framed window layout from
  docs/tasks/window-frame.md can be switched off by a query parameter, e.g.
  `?embed`), accepts parameters for the intro (`?nointro`), a starting command and
  the theme, pauses when not visible (IntersectionObserver; the idle throttling
  already keeps hidden/idle cost low), and documents the recommended `sandbox` and
  `allow` attributes. Document the embedding snippet in the README of the demo.

Still open:

- Name and place of the native executable (`matterm-fakeos` assumed) and whether
  it ships in the default build or behind an option.

Future direction (not part of this task): chatting with other visitors in the web
demo. That needs a relay server, so it conflicts with pure static hosting. The
shape to design later is *apps* that are written against a small program
interface (this task's `Program`/core), run statically in the browser, and
optionally talk to a thin relay for multi-user features. Keep the core free of
assumptions that would block that (no globals, injected time and randomness,
input and output as byte streams).

## Handoff

As in docs/PARALLEL_WORK.md: full commit SHAs in dependency order per milestone,
changed files, API changes (especially the `ScriptedLink::Program` extension),
commands run and results, limitations, deferred items. Do not push.
