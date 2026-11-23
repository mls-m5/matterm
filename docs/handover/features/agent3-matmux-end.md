# End a terminal from the matmux list

Written by agent3, 2026-11-23.

## What it does

In the list (`matmux`, `matmux --attach`, `--rendered`, `matterm --attach-tty`): `x` (or Delete) on a
terminal row asks in the note line: `End "matterm" (vim)? y = end, n/Esc = keep`. The question names the
terminal and what runs in it, and adds `(also open in another client)` when another client has it. **Only `y`
ends it**; Enter, Escape, `n`, `q`, an arrow or any other key keeps it (the default is no). The terminal this
program runs in (`MATTERM_SESSION`) is refused with a note ("You are in this terminal: ending it would end
this program too"); the New terminal row does nothing. After ending the list is read again, the chosen row
stays near where the terminal was (the next one, or the last one, or New terminal when none is left) and the
note says "Ended matterm". An error (an older daemon, the terminal gone) is shown in the note line and
nothing changes. `x end` is in the footer; README and REMOTE_DAEMON have the line.

## Where

`src/frontend/ttylist.h` (the question as part of the pure list model: `ListKey::End/Yes`,
`endQuestion()`, `anchor`), `src/frontend/ttyattach.h` (`SessionSource::end`, the action),
`app/attachtty_impl.h` (`DaemonSource::end`).

## The daemon call

`DaemonSource::end` calls agent2's `frontend::endSession` (control request EndSession, additive 1.3.4);
a daemon older than that says so in the note line ("this daemon is older than 1.3.4 ..."). The daemon may
list the terminal for a few tens of milliseconds after it accepted the request, so the list drops the
ended id itself.

## About the program shown for a terminal started with -- COMMAND

Not a race: `sh -c ...` looks like a shell to the daemon's reading of /proc (busy means "the foreground
process is not a shell"), so such a terminal shows `shell`. A program that is not a shell shows (`--new
-- sleep 61` is listed as `sleep 61`; `matmux_pty.py` checks it). The command could not be taken from the
request, because another client's list does not know it.

## Verified / not verified

`test/ttylist_test.cpp` (the question text, default keep, y ends, y without a question does nothing, own
terminal refused, New terminal row, another client noted, selection near after ending, the question drawn,
x/Delete/y mapped), `test/ttyattach_test.cpp` (x then y ends and refreshes, Delete asks, Enter/Esc/n/q/j keep,
an error from the daemon stays in the note line, own terminal not ended). `test/matmux_pty.py` against a real daemon: x asks naming the terminal, Enter keeps it, the question names
the program (`sleep 62`), y ends a busy terminal and an idle shell (the daemon's list shrinks), the own
terminal (MATTERM_SESSION) is refused. Not verified: an older daemon in practice (unit-tested with a fake),
a phone.
