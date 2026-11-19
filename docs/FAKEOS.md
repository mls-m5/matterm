# fakeos: a pretend Unix with ASCII animations

`src/fakeos/` is a small, self-contained "operating system" that exists only in
memory: a virtual file system, a shell, about forty commands, full-screen ASCII
programs, and a few easter eggs. It is the content of the static browser demo
(`index.html?demo`, no server) and is meant to run unchanged in other hosts.
Nothing in it can reach the real system: `rm -rf` removes virtual files only, and
`reset` (or a reload) restores the first state.

Status: milestone 1 of [tasks/fakeos-demo.md](tasks/fakeos-demo.md) (core, `fire`,
intro, browser host). The native host, renames, more animations, and static
packaging follow in later milestones.

## What a visitor sees

1. **Intro** (about 3 s): "matterm" resolves out of noise in block letters; any
   key or click skips it (`?nointro` turns it off).
2. **A prompt** in a small Unix: `guest@matterm:~$`, the motd, a home directory
   with `README`, `notes.txt`, `projects/` and hidden files.
3. **The shell**: quotes, `$VARS`, `~`, wildcards (`*` `?` `[a-z]`), pipes `|`,
   `;` `&&` `||`, redirects `>` `>>` `<`, history (Up/Down), cursor editing,
   Ctrl-A/E/U/K/W/L/C/D, Tab completion (a second Tab lists the choices). The line
   stays on one terminal row; typing past the width is refused.
4. **Commands**: `cd pwd echo export unset history clear reset exit help true
   false` (builtins) and `ls cat mkdir touch rm cp mv head tail wc grep sort uniq
   seq yes find tree uname whoami id hostname date env ps uptime colors which man`
   (files in `/bin`). Errors and exit codes follow the real tools; permissions
   apply (`/etc` belongs to root, `rm /etc/passwd` is refused).
5. **Programs on disk.** Full-screen programs are files too: `/usr/games/fire`
   is a file whose content is `#!builtin fire`. `/usr/games` is not in the
   `PATH`, so `fire` alone is "command not found" until the visitor looks around.
   A program takes the screen (alternate screen, hidden cursor), draws at about
   30 frames per second, and returns to the shell on `q`, Esc or Ctrl-C. What it
   did to the world stays.

[FAKEOS_EGGS.md](FAKEOS_EGGS.md) lists the discoveries (spoilers).

## Design

```
bytes in --+
resize  ---+--> fakeos::System --> bytes out (plain VT)
tick(now) -+        |
                    +--> nextDeadline() for the host's loop
```

The core does no I/O: no files, processes, threads or clocks. Time arrives as
`tick(now)`, randomness comes from a seeded generator (`Config::seed`), and the
wall clock for `date` is `Config::unixTime` plus elapsed time, so tests and
screenshots are reproducible. `test/check_fakeos_includes.cmake` (ctest
`fakeos_includes`) fails the build if `src/fakeos` includes an I/O, time, thread,
process or platform header or calls such an API.

| Part | File | Role |
|---|---|---|
| `System` | `system.*` | The pretend terminal session: intro, line editor, prompt, runs programs, restores the screen. `start()`, `input()`, `tick()`, `resize()`, `nextDeadline()` return `Reaction {output, exitCode}`. |
| `World` | `world.*` | All state: file system, cwd, environment, history, clock, RNG, discoveries (`flags`), pending requests. Programs and eggs may read and change it. No globals. |
| `VirtualFs` | `virtualfs.*` | In-memory tree with owners and modes (owner/other only), and limits (2000 nodes, 1 MiB total, 256 KiB per file, depth 32, 255-byte names) so a runaway `yes > f` stops at a limit. |
| `Shell` | `shell.*` | Tokenizer, expansion, pipelines, redirects, program lookup through the file system, completion. |
| Commands | `commands.*` | Pure functions `(World&, args, CommandIo&) -> status`; the table says which are builtins and which exist as `/bin` files. |
| `Program` | `programs.h`, `fire.cpp`, `gradient.cpp`, `intro.cpp` | Full-screen programs: `begin`, `key`, `frame`, `end`, and an optional farewell. They draw into an `AsciiCanvas`. |
| `AsciiCanvas` | `canvas.*` | Cell buffer; `flush()` emits only changed cells with the smallest cursor and color updates (an unchanged frame is empty). Colors are reduced to truecolor, 256 or 16 colors per `Capabilities`. |
| `KeyDecoder` | `keys.*` | Terminal input to keys: UTF-8, control and CSI/SS3 keys, SGR mouse reports, bracketed paste; replies are dropped. |
| Eggs | `eggs.*` | A table of triggers and effects. |

`Capabilities {truecolor, colors256, alternateScreen, unicode}` is in `Config`; a
host that cannot do something says so and the programs degrade (256 or 16 colors,
screen clear instead of the alternate screen). Only standard ECMA-48/xterm
sequences are emitted.

### Hosts

- **Browser** (`web/main.cpp`, no-server branch): `fakeos::ScriptedProgram`
  (`scriptedprogram.h`, header-only, so the library does not depend on the
  transport) runs the `System` as a `ScriptedLink::Program`. `ScriptedLink` gained
  `Program::tick()` and `nextDeadline()` for it; the browser loop's idle throttling
  wakes up as soon as a frame changes the picture.
- Later milestones: the native `matterm-demo` window and an optional raw-tty mode.

### Adding things

- **A command**: write the function in `commands.cpp` and add a row to the table.
  Non-builtin rows get a `/bin/NAME` file automatically (so deleting the file
  removes the command until `reset`).
- **A program**: implement `Program`, add a factory to `programs.cpp`, and put a
  file `#!builtin NAME` somewhere in `seedWorld` or in an egg.
- **An easter egg**: add a row to `eggs()` (see FAKEOS_EGGS.md).

## Tests

`test/fakeos_*_test.cpp` (ctest `fakeos`): key decoding, canvas diffing and color
reduction, the file system and its limits, the shell and every command through a
real `VtParser`/`TerminalModel` pair with a fake clock, programs and eggs
(determinism per seed, the discovery chain, intro skipping, small terminals,
terminals without the alternate screen or colors), and the `ScriptedLink` adapter.
`web/tools/e2e-demo.js` runs the browser demo (intro, commands, fire).

## Limits

No job control, functions, loops or `if` in the shell; no `sudo` (it is an egg);
`yes` stops after 2000 lines; the prompt line cannot wrap; wide characters count as
one cell like the rest of the terminal model. The virtual world is not saved
between visits.
