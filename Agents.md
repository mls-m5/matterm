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

The project name is `matterm` (lowercase). Keep `matterm-demo` as a standalone
library usage example, separate from the future terminal emulator. Demo artwork
and generated content belong under `demo/`, not in the reusable renderer.
Keep `matterm-terminal-demo` as a second, separate client of the CPU-only
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
