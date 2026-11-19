# Terminal Simulator

`ScriptedLink` (`src/transport/scriptedlink.h`) is the third `TerminalLink`
implementation, next to `ClientSession` (remote server) and `PtyLink` (local
PTY). It behaves like a terminal program without any process or network, so:

- the browser page can run a self-contained, interactive demo when no server
  URL is given (`web/main.cpp`), safe to embed on a public page;
- front-end behavior (parser, model, input encoding) can be tested without a PTY.

It is deterministic: time comes from an injected clock (`Config::clock`, like
`ClientSession`), nothing sleeps or blocks, and there are no threads. The front
end calls `tick()` each frame and may use `nextDeadline()` to schedule the next
call. All callbacks run inside `tick()`, never inside `sendInput()` or
`resize()`, so a reply the parser writes while consuming output cannot re-enter
the link. Input is queued (up to `Config::maxPendingInput`, default 64 KiB); the
exit is reported once, after which `sendInput()` returns false.

## Scripts

A `Script` is a list of steps, built with chained calls:

```cpp
Script script;
script.output("login: ")
    .waitForInput("\r")
    .output("Welcome\r\n", /*bytesPerSecond=*/40)
    .delay(std::chrono::milliseconds(500))
    .exit(0);
ScriptedLink link(config, std::move(script));
```

| Step | Meaning |
|---|---|
| `output(text, bytesPerSecond = 0)` | Emit text; with a rate the text streams, otherwise all at once. |
| `delay(ms)` | Wait. |
| `waitForInput(literal)` | Wait until the input contains `literal`; consume up to the match. Later bytes stay queued. |
| `waitForInput(predicate)` | Wait until the predicate accepts the input received since the last wait; consume it all. |
| `exit(code)` | End the session with this exit code. |

Step times are nominal: a step starts when the previous one was *due* to end
(or when the input that released a wait arrived), so the output does not depend
on how often `tick()` is called.

## The interactive simulator

`ScriptedLink::simulator(config)` runs the `Simulator` program: a prompt, echo
of typed text with Backspace, Enter, Ctrl-C, Ctrl-U and Ctrl-D, and the
commands `help`, `echo`, `clear`, `colors`, `size` and `exit [n]`. Everything
it writes is plain VT output, so it exercises the real parser. It ignores
escape sequences in its input (arrow keys, terminal replies, paste markers) and
never answers them. `resize()` changes the width it lays out for (`size`,
`colors`, the maximum line length, which keeps the line on one row).

A script can lead into the simulator: pass a `Script` and a
`std::make_unique<Simulator>(columns, rows)` to the `ScriptedLink`
constructor; the program takes over when the script ends without `exit()`.
Any `ScriptedLink::Program` (pure `start`/`input`/`resize` returning output
and an optional exit code) can be used the same way.

## Limits and follow-ups

- Scripts exist only in C++. Loading them from files (JSON), or from asciinema
  or `--record-output` recordings, is a follow-up.
- Paced output may split a UTF-8 sequence across calls; the parser handles that.
- The simulator has no history, cursor movement within the line, or tab
  completion.
