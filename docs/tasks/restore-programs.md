# Offer to start the saved programs again (agent3)

Owner's wish (2026-11-23): after "Open session", ask once "Start 3 programs again?" instead of
only offering each command on its own prompt line.

Decided
- Today a saved pane keeps the command of its foreground program (`SavedPane::command`,
  `src/frontend/sessionrestore.h`, `rerunInput`); restore never starts it, it is only offered on
  the prompt line. That stays the default and the fallback.
- New: after "Open session" (not for Attach tab, not for the browser), when at least one restored
  pane has a saved command, one question: "Start N programs again?" with the programs listed
  (command and short folder per line, the same shell quoting as the offer; a long list scrolls).
  Answers: **Start them** (types each command and Enter into its pane, like `rerunInput` plus
  Enter) and **Not now** (the default; the prompt-line offers stay as they are today). No "choose
  which" in this version.
- Safety: only commands that `plainCommand` accepts (as now); nothing is started without the
  answer; a pane whose folder is gone or whose terminal could not be created is left out of the
  count and the list. Works with a local and a remote daemon.
- Tests: the question text and count (pure, in a header), the typed input, the default answer is
  "Not now", a pane without command or with a refused command is not counted.
- Feature description in docs/handover/features/agent3-restore-programs.md, SHAs to the integrator.
