# "Start N programs again?" after Open session

Written by agent3, 2026-11-23. Contract: docs/tasks/restore-programs.md.

## What it does

After **Open session...** one question: "Start 3 programs again?" with the programs listed (short
folder and the shell-quoted command per line; a long list scrolls with Up/Down/PageUp/PageDown or the
wheel). **Start them** types each command and Enter into its pane; **Not now** (the default, also
Escape) leaves things as before: the note "was running: vim x - Enter to run it again" and the command
typed on the prompt line without Enter. Counted: panes with a saved command that `plainCommand`
accepts, whose folder is still there and whose terminal is live. Panes that are not counted keep the
prompt-line offer. Not asked for Attach tab, not in the browser. Works with a local and a remote daemon.
Nothing is started without the answer.

## Found on the way

For a daemon pane (the default) the note and the typed command of the old offer were sent at once,
while the pane was still connecting: the input is refused then and the note is wiped by the replay, so
the offer did not appear. Notes and typed text for restored panes (folder gone, previous screen, the
offers, "starting again") now wait until the pane's connection runs (`deliveries` in ptyfrontend.cpp,
dropped after 15 s).

## Where

`src/frontend/restoreprograms.h` (who is counted, question, lines, what is typed, action ids),
`src/frontend/ui/programlist.h` (the dialog), `app/ptyfrontend.cpp` (restoreByName, handleAction).

## Verified / not verified

`test/restoreprograms_test.cpp` (count and singular, not counted: no command, refused command, folder
gone, no terminal; the line; typed text ends with Enter; default Not now, Escape, Tab, click, scrolling,
a tiny window). `layout_smoke.py` variants `programs` and `programs-skip` open a saved session with two
programs in the real window code, local daemon and `--daemon-host` through the fake ssh: the question
counts 2, Start them types them, Not now leaves the offers and starts nothing. Not verified: a real
display (the dialog's look), a real shell that is slow to start (the typed text waits in the tty), "choose
which" (not in this version).
