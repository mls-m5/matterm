# Attaching closes the empty shell in front

Written by agent3, 2026-11-23.

## What it does

In the window, when **Attach terminal...**, **Attach tab...** or **Attach all leftover terminals** has worked
(a new tab exists), the tab that was in front when the command ran is closed if it is an *empty shell*, and
its fresh terminal ends in the daemon (closing a pane ends its terminal); the notice says "empty shell
closed" (for Attach tab: "layout 'x' attached; empty shell closed"). Nothing is lost: nothing was typed,
nothing runs, nothing scrolled by. Only that one tab, only after the attach worked (a failed or cancelled
attach closes nothing; the take-over question comes before, the check is made when the attach really runs);
never the last tab (the new tab exists first); not for **Attach terminal here...** (it replaces a pane).

## The rule (`src/frontend/emptyshell.h`, `isEmptyShell`)

All of: the tab has exactly one pane; no byte was sent to its terminal by the owner since it started
(`TerminalView::inputBytes()`: keys, paste, mouse reports; the terminal's own answers to queries do not
count); what runs there is known and is not a program (the daemon's `GetSessionInfo` busy flag, or the probe
of a local pane; a daemon that did not answer counts as "not known" and keeps the tab); no scrollback rows
(`TerminalModel::historySize()`); no copy mode or selection.

Question for the owner, with alternatives (the task allowed it): is "typed but erased" empty? Today no: any
typed byte keeps the tab. (a) keep that (nothing guessed: the safe one, recommended); (b) also close when the
typed text was erased (needs the shell's line, which matterm cannot read: it would have to guess from the
screen); (c) also close when only Enter was pressed (a blank prompt line): the screen shows only prompts.

## Verified / not verified

`test/emptyshell_test.cpp` (the rule: fresh, typed 1 byte or paste, busy, unknown, scrollback, selection,
more than one pane). `layout_smoke.py` variants `emptyshell` and `emptyshell-typed` (hidden window, real daemon,
local and through the fake ssh): a fresh shell is closed after `Attach terminal`, the attached terminal is in
front, the daemon has one terminal less; with a byte typed first both tabs and both terminals stay. Not
verified: Attach tab... and Attach all with an empty shell in front (the same two calls, not driven), a real
display.
