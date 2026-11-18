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
