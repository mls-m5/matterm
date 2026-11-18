# Agent Instructions

When creating or modifying project files, update `.2026-terminal-renderer.files`
to reflect the files in the project. Add new project files and remove entries
for deleted files.

Commit after each significant, coherent change. Do not include unrelated local
changes.

Write unit tests using the framework in `test/mls-unit-test`.

Write project documentation in English.

Run clang-format on all project C++ source and header files before committing.
Do not reformat third-party submodules.
