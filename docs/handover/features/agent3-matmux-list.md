# The matmux list: folder names first, New terminal chosen by default

Written by agent3, 2026-11-23.

## What changed

- **First column = what the terminal is about.** Each row starts with the name of the terminal's top
  folder: basename of its cwd (`~/projects/matterm` -> `matterm`; `/` for the root, an empty cwd and when the
  folder is unknown to the daemon's answer; a trailing slash does not count; control characters become
  blanks). An explicit name (`--name`, Rename terminal...) wins. Then what runs (the command, or `shell`)
  and the full folder. When the daemon did not answer at all, the row says `terminal <id8>` as before.
  Rule: `folderName()` and `makeListEntry()` in `src/frontend/ttylist.h`.
- **`matmux` without arguments** shows the list with **New terminal** chosen (Enter starts a terminal with
  your shell); when no terminal runs at all it starts one directly (no pointless list; the daemon is started
  when missing, as before). The old line "N terminals run: matmux --attach" is gone (the list shows them).
  `matmux --new [...]` still starts directly; `matmux --attach` is the list with the first existing terminal
  chosen. Both work in `--rendered` too; `matterm --attach-tty` is unchanged (first terminal chosen).
  Mechanics: `ListModel::setPreferNew`, `AttachTty::Options::selectNew`, `DaemonOptions::attachDefault`.
- Completion and `--help` text updated (`--attach` describes the new rows' default).

## Verified / not verified

`test/ttylist_test.cpp` (the name rule incl. trailing slash, root, empty, relative, control characters and
that a name wins; the three columns in a drawn row; preferNew at start, after a refresh and when a terminal
was chosen), `test/ttyattach_test.cpp` (selectNew: Enter creates, Down then Enter opens),
`test/matmux_pty.py` (real daemon, real ptys: a terminal started in a folder is listed by its folder name, a
named one by its name, then `shell` and the full folder; no arguments with terminals running shows the list and
Enter adds exactly one terminal; `--attach` Enter opens the first terminal and adds none) and
`test/matmux_test.py` (no arguments and no terminal: no list, a terminal at once; then the list with New
terminal). Not verified: a phone; what the list shows for the program of a terminal started with
`-- COMMAND` is `shell` in the daemon's answer here (`busy` is the daemon's reading of /proc), not looked into.
