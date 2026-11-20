# Agent Instructions

When creating or modifying project files, update `.matterm.files`
to reflect the files in the project. Add new project files and remove entries
for deleted files.

Commit after each significant, coherent change. Do not include unrelated local
changes.

Write unit tests using the framework in `test/mls-unit-test`.

Write project documentation in English.

Run clang-format on all project C++ source and header files before committing.
Do not reformat third-party submodules.

The project name is `matterm` (lowercase). Keep `matterm-render-demo` as a
standalone library usage example (renderer only), separate from the terminal
emulator. Demo artwork and generated content belong under `demo/`, not in the
reusable renderer. The name `matterm-demo` is the showcase (pretend OS, animations,
easter eggs; see `docs/tasks/fakeos-demo.md`). Keep `matterm-lib-demo` as a second, separate client of the CPU-only
`matterm::terminal` model. Scrollback and primary/alternate modes must remain
available to library users, not only to a future terminal executable.
Keep both existing demos' behavior intact; VT/PTY integration belongs in the
separate `matterm` application. Track implementation progress in `ROADMAP.md`.
Common types/storage belong in `src/core/`, graphical backends in `src/render/`,
and terminal emulation/stream parsing in `src/terminal/` and its `vt/` subdirectory.
The reverse-direction ANSI output backend idea is deferred, not current work.

Before parallel implementation, use `docs/PARALLEL_WORK.md` for task ownership,
base commits, separate clones/worktrees, and handoff/review requirements. Candidate
independent tasks are listed there. Settings, profiling/benchmark boundaries, and
future staged pipelines are recorded in `docs/SETTINGS_AND_PERFORMANCE.md`.
Do not modify user-staged submodule additions incidentally. Documentation changes
inside matperf are authorized; commit those separately and report their SHA.

The operator authorizes subagents for small, well-scoped tasks such as independent
tests, documentation updates, or focused reviews. Prefer direct execution for
simple changes; delegate only independent work with clear boundaries. The operator
reports a smaller model named `luna-6`; verify available agents/model configuration
before using it rather than assuming that this identifier is configured. Read the
pi-subagents skill before delegation. The main agent reviews delegated changes
and runs integration tests before committing.

## How we work (integrator and agents)

- One integrator (the session that owns `main` and `web/` integration) and any
  number of agents. Each agent has its own git worktree under `.tmp/agentN` and
  its own branch from the current `main`. Agents commit on their branch only and
  never push; the integrator merges, runs the full suite and pushes.
- Every task has a contract in `docs/tasks/<name>.md` (goal, scope, out of scope,
  owned and shared paths, acceptance) and an entry under Active Tasks in
  `docs/PARALLEL_WORK.md`. Agents stay inside their owned paths and report edits
  to shared files. Tell the integrator plainly what could not be run.
- Tests first: a bug fix starts with a test that fails, in its own commit, then
  the fix. Keep commits small and coherent. Run clang-format before committing.
- Tools differ per sandbox. The integrator's sandbox has Emscripten (`~/emsdk`)
  and headless Chromium with Playwright (`~/pw`, run with
  `NODE_PATH=$HOME/pw/node_modules`). Agents may lack them, so browser
  behavior must be verified by the integrator with `web/tools/run-e2e.sh`.
- Agents communicate through the MCP message tools (`list_peers`,
  `send_message`, `receive_messages`), not through subagents. Messages are short
  and concrete; no acknowledgements.
- Build directories: `build/` (default, with a separate `build/server` configured
  with `-DMATTERM_BUILD_SERVER=ON`) and `build-web/` (Emscripten; not committed).
  `matterm-server` reads its static files at startup, so restart it after
  rebuilding the web client.
- Submodule worktrees are separate clones. If `git status` shows `M lib/...` in a
  worktree, check whether it is only an older checkout (`git submodule status`);
  fetch the pointed commit from the main repo's submodule with
  `git -c protocol.file.allow=always fetch <main>/lib/<name>` instead of editing
  files there. Never commit a submodule pointer by accident.
- Pushes go through the host approval tool with a full commit SHA, one push per
  approval. Use `pkill -x name`, never `pkill -f`.
- `ROADMAP.md` tracks progress; designs for larger features are written in
  `docs/` and agreed before implementation (see "Later" in the roadmap).
