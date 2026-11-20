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
- **Naming and one demo program** (decided by the owner; do the renames in the
  native-host milestone, as a separate commit, updating Agents.md, README.md,
  docs/FONTS.md, CMake and any scripts together; the owner authorizes changing the
  earlier "keep matterm-demo standalone" rule):
  - current `matterm-demo` (renderer only) is renamed **`matterm-render-demo`**;
  - current `matterm-terminal-demo` (the model's direct API) is renamed
    **`matterm-lib-demo`**;
  - **`matterm-demo`** becomes the new showcase: fakeos is merged into it, so there
    is one concept, not two. It shows the intro animation, runs the pretend OS
    (virtual files, shell, programs) and the ASCII animations, and has the easter
    eggs below. Hosts: the SDL window through the matterm model/renderer (default)
    and the web build; the raw-tty variant (formerly `matterm-fakeos`, runs inside
    any terminal) is kept as a mode of the same program if it stays cheap
    (`matterm-demo --tty`; confirm in milestone 2, otherwise drop it and say so).
- **One shared experience, not modes**: animations and the pretend OS are the same
  world. The visitor explores a small operating system (files, programs, hidden
  places) and the animations are part of it: they are programs, files and
  discoveries inside that world (the intro boots into it, `fire` can be a program
  found on disk, an easter egg can trigger a gradient or a crumble, finishing an
  animation returns to the shell with a changed world). There is no "animation
  mode" switch and no separate menu of demos; whatever is reachable is reachable
  by exploring, so the exploration itself must be rewarding and a little
  mysterious. Design the core around that: animations are `Program`s started by
  the shell or by triggers, and they can read and change the virtual world.
- **Easter eggs** are a first-class feature of the core: hidden content that is found
  by exploring: typing certain words or commands, finding the right files or
  programs in the virtual file system (hidden files, odd directories, a program
  that only appears after another is run), and clicking the right spots through
  mouse reporting. Design them as data (a table of triggers and effects) so more
  can be added without touching the shell; keep them deterministic (seeded) for
  tests; list them in a spoiler-marked document, not in `--help`.
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

- Whether `matterm-demo` ships in the default build or behind an option (the
  `matterm-lib-demo` and `matterm-render-demo` renames are decided).

Future direction (not part of this task): chatting with other visitors in the web
demo. That needs a relay server, so it conflicts with pure static hosting. The
shape to design later is *apps* that are written against a small program
interface (this task's `Program`/core), run statically in the browser, and
optionally talk to a thin relay for multi-user features. Keep the core free of
assumptions that would block that (no globals, injected time and randomness,
input and output as byte streams).

## Finish the demo (current assignment)

The owner wants the demo finished: milestones 2 to 4, in this order, reporting after
each. The integrator separately handles the GitHub Pages pipeline and the static
`demo.html` page (generated from `web/shell.html`, same files as the browser
client); do not build packaging or CI yourself, but keep the web demo working with
only static files (no server) and tell the integrator what the page needs.

- **Milestone 2**: native `matterm-demo` (SDL window through the model/renderer,
  with the framed/cell-snapped behavior of the other frontends, `--no-intro`),
  optional `--tty`, click easter eggs (`Egg::Trigger` for mouse clicks via
  mouse reporting).
- **Milestone 3**: more commands, more animations and easter eggs, including the
  Matrix egg below; the files and programs that lead to them.
- **Milestone 4**: polish and tests (determinism per seed, e2e in `e2e-demo.js`),
  `docs/FAKEOS_EGGS.md` (spoilers), README for the demo including the iframe
  embedding snippet and the `?embed`/`?nointro` parameters.

### The Matrix egg (owner's spec)

Somewhere in the virtual file system the visitor can find something that refers to
*The Matrix* (a file, a program or a note; hidden well enough to be a discovery,
reachable by normal exploration, with a hint trail). Following it triggers, as one
scripted sequence inside the same world:

1. The screen goes black.
2. The text `Hello, Neo.` appears (typed out) with a blinking cursor.
3. After a pause: `Follow the white rabbit.`
4. Green characters rain down the screen like the Matrix digital rain (columns of
   falling glyphs with a bright head and fading tail; seeded randomness; resizes
   correctly; any key skips).
5. After a while the rain ends and the visitor returns to the **intro animation**,
   which then leads to the shell prompt again (the world keeps its changes).

Timing is data (named constants), the whole sequence is a `Program`, and it is
tested deterministically with the fake clock like the other animations.

## Handoff

As in docs/PARALLEL_WORK.md: full commit SHAs in dependency order per milestone,
changed files, API changes (especially the `ScriptedLink::Program` extension),
commands run and results, limitations, deferred items. Do not push.
