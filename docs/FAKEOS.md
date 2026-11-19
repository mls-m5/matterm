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

1. **Intro** (about 7 s): "MATTERM" resolves out of noise in block letters, the line
   "Futuristic system agnostic terminal emulator" is typed, scrambles into noise and
   vanishes letter by letter, and "Real terminal, fake operating system" takes its
   place. A red "DEMO MODE" blinks in the top right corner for a few seconds and
   goes away. The background moves gradually from the intro's own dark color to the
   terminal's default one, so the prompt appears without a jump. Any key or click
   makes the text go quickly (0.3 s) but the background still glides (1.2 s): the
   intro never cuts. `?nointro` / `--no-intro` turn it off. The timings are named
   constants at the top of `src/fakeos/intro.cpp`.
2. **A prompt** in a small Unix, **Mattrix OS 6.0 Professional Edition** (`uname -o`,
   `/etc/os-release`, the motd, `neofetch`): `guest@matterm:~$` (the machine is
   called matterm), a home directory with `README`, `notes.txt`, `projects/` and
   hidden files. `neofetch` (alias `sysinfo`) draws an old fat CRT television beside
   the system information.
3. **The shell**: quotes, `$VARS`, `~`, wildcards (`*` `?` `[a-z]`), pipes `|`,
   `;` `&&` `||`, redirects `>` `>>` `<`, history (Up/Down), cursor editing,
   Ctrl-A/E/U/K/W/L/C/D, Tab completion (a second Tab lists the choices). The line
   stays on one terminal row; typing past the width is refused.
4. **Commands**: `cd pwd echo export unset history clear reset exit help true
   false` (builtins) and, as files in `/bin`, `ls cat mkdir touch rm cp mv head
   tail wc grep sort uniq seq yes find tree uname whoami id hostname date env ps
   uptime colors which man cal fortune cowsay rev tac nl tr cut sed printf basename
   dirname sleep df free less top neofetch sysinfo`. Errors and exit codes follow the real tools;
   permissions apply (`/etc` belongs to root, `rm /etc/passwd` is refused). `less`
   and `top` are full-screen programs: `less FILE` (or a pipe) pages with
   j/k/Space/b/g/G and the mouse wheel; `top` shows drifting numbers.
5. **Programs on disk.** Full-screen programs are files too: `/usr/games/fire`
   is a file whose content is `#!builtin fire`. `/usr/games` is not in the
   `PATH`, so `fire` alone is "command not found" until the visitor looks around.
   A program takes the screen (alternate screen, hidden cursor), draws at about
   30 frames per second, and returns to the shell on `q`, Esc or Ctrl-C. What it
   did to the world stays. Programs can chain: a program or a click egg may ask
   for the next one (the red pill leads to the Matrix, which leads back to the
   intro). Some programs are hidden by a dot (`ls -a /usr/games`).

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
| `Program` | `programs.h`, `fire.cpp`, `gradient.cpp`, `intro.cpp`, `matrix.cpp`, `crumble.cpp`, `less.cpp`, `top.cpp` | Full-screen programs: `begin`, `key`, `frame`, `end`, and an optional farewell. They draw into an `AsciiCanvas`. |
| `AsciiCanvas` | `canvas.*` | Cell buffer; `flush()` emits only changed cells with the smallest cursor and color updates (an unchanged frame is empty). Colors are reduced to truecolor, 256 or 16 colors per `Capabilities`. |
| `KeyDecoder` | `keys.*` | Terminal input to keys: UTF-8, control and CSI/SS3 keys, SGR mouse reports, bracketed paste; replies are dropped. |
| Eggs | `eggs.*` | A table of triggers and effects. |

`Capabilities {truecolor, colors256, alternateScreen, unicode}` is in `Config`; a
host that cannot do something says so and the programs degrade (256 or 16 colors,
screen clear instead of the alternate screen). Only standard ECMA-48/xterm
sequences are emitted.

### Pace of the output

`Config::revealCharsPerSecond` (the demos use 2500, about 25 lines a second; the
default 0 prints at once, which is what the tests use) makes the output of
commands, `help` and the welcome text appear at a pace: a character costs one
unit, a new line twelve, escape sequences nothing, so colors never split. Short
texts (a line or two and the prompt) are not held back, a long text is shown within
four seconds whatever the rate, and programs wait until the text is out. Space,
Enter, Escape or Ctrl-C show the rest at once; any other key does too and is then
typed, like type-ahead. Mouse reports are ignored meanwhile. Typing, echo and
prompts are never delayed. `--no-reveal` and `?noreveal` turn it off.

### Hosts

- **Browser** (`web/main.cpp`, no-server branch): `fakeos::ScriptedProgram`
  (`scriptedprogram.h`, header-only, so the library does not depend on the
  transport) runs the `System` as a `ScriptedLink::Program`. `ScriptedLink` gained
  `Program::tick()` and `nextDeadline()` for it; the browser loop's idle throttling
  wakes up as soon as a frame changes the picture.
- **Native window** (`matterm-demo`, `app/demo/main.cpp`): the same `ScriptedProgram`
  behind a `ScriptedLink`, shown through `TerminalModel`, `VtParser` and the SDL
  renderer. Like the other frontends it snaps the window to whole cells, reserves
  the scrollbar strip, reports the mouse to programs that ask for it (the click
  eggs), and supports selection (Ctrl+Shift+C/V/A), wheel and Shift+PageUp/Down
  scrolling. `exit` closes it. Options: `--no-intro`, `--seed N`, `--font-size`,
  an optional font path, `--smoke-test` (hidden window self-test).
- **Plain terminal** (`matterm-demo --tty`, `app/demo/ttyhost.cpp`, POSIX only):
  the same core on this process's own terminal: raw mode, `poll` on stdin with a
  timeout from `nextDeadline()`, `SIGWINCH` through a self-pipe, and the terminal
  restored on every way out (also on SIGINT/SIGTERM/SIGHUP). Capabilities come
  from the environment: truecolor from `COLORTERM`, 256 colors from `TERM`, no
  alternate screen for `TERM=linux`/`dumb`/`vt100`/`vt220`, Unicode from the
  locale. It refuses to run unless stdin and stdout are terminals, and opens
  nothing but them. Tested under a pseudo-terminal (`test/demotty_test.cpp`).

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
