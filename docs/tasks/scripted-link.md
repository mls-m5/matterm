# Task: ScriptedLink, a terminal simulator behind TerminalLink

Assignment for an independent agent in its own worktree and branch (`.tmp/agent3`,
branch `agent3-scripted`). Read first: Agents.md, docs/PARALLEL_WORK.md (task
contract, handoff, environment notes), docs/WEB_TERMINAL.md (the layering
section: `MessageChannel` vs `TerminalLink`), `src/transport/terminallink.h`,
`src/transport/clientsession.h` (the remote implementation) and
`src/platform/posix/ptylink.h` (the native one). The front end never sees which
link it has; this task adds the third implementation.

## Goal

A `ScriptedLink` that behaves like a terminal program without any process or
network, deterministically, so that the browser build can run a self-contained
demo ("terminal simulator") and front-end behavior can be tested without a PTY.

## Scope

1. **`ScriptedLink`** in new files `src/transport/scriptedlink.h/.cpp`: implements
   `TerminalLink`. Time is injected like in `ClientSession` (a clock in its
   config, a `tick()` the front end calls each frame, `nextDeadline()`), so tests
   use a fake clock. No threads, no sleeping, no I/O.
2. **A script format** the link plays: a list of steps in C++ (no file format
   needed yet): `output(text)`, `delay(ms)`, `waitForInput(literal or predicate)`,
   `exit(code)`. Output can be paced (`bytesPerSecond`) to show streaming. A
   `Script` builder keeps tests short.
3. **A small interactive simulator** built on the same link: prompt, echo of
   typed text with backspace and Enter, and a handful of built-in commands
   (`help`, `echo`, `clear`, `colors`, `exit [n]`), written as VT output so it
   exercises the real parser. Honor window size changes (`resize()`), and answer
   nothing the real program would not.
4. **Wire into the browser demo**: in `web/main.cpp` replace the built-in demo
   text, used when no server URL is given, with the simulator, so the browser
   page without a server is interactive. Keep that change small; the integrator
   edits the rest of `web/main.cpp` in parallel, so touch only the block that
   creates the demo link.
5. **Docs**: a short section in docs/WEB_TERMINAL.md (or a new
   docs/SIMULATOR.md) on what ScriptedLink is for and its script steps.

## Out of scope

Real shells or processes, loading scripts from files or asciinema recordings
(note it as a follow-up), changes to `TerminalLink`, `ClientSession`, the
protocol, `src/terminal/`, and the server.

## Owned and shared paths

Owned: `src/transport/scriptedlink.*`, `test/scriptedlink_test.cpp`,
`docs/SIMULATOR.md`. Shared, edit minimally and report: root `CMakeLists.txt`
and `test/CMakeLists.txt` (new target and test, following `ttytransport`),
`web/CMakeLists.txt` (source list), `web/main.cpp` (the demo block only).

## Acceptance

- Tests with a fake clock: output pacing, delay, waitForInput, exit reported
  once, input after exit is refused, resize reaches the simulator, backspace and
  Enter editing, each built-in command (check the produced bytes, and feed them
  through `VtParser` into a `TerminalModel` to assert the screen text).
- The full suite passes in the default configuration.
- The browser build still compiles (`emcmake`, see web/README.md) and, with no
  `?ws=` and no server, the page shows the prompt and responds to typed
  commands (use `web/tools/shot.js` or `e2e-input.js` for a screenshot).
- clang-format applied; no build products committed.

## Handoff

As in docs/PARALLEL_WORK.md: full commit SHAs in dependency order, changed files,
commands run and results, limitations, deferred items. Do not push.
