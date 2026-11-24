# Detach from the window-close confirmation

Tests first: `0d2c2a9`. Implementation and native smoke: `4007b51`.
Base: integrated main `baa0bf6`.

When a window-close request needs confirmation because programs are running,
the dialog offers **Close / Detach / Cancel** only if every live pane has an
identified daemon session and no local PtyLink. Ended panes do not prevent
safe detach; an empty/all-ended window does not qualify. Hidden tabs count too.
A live local Settings TUI therefore prevents the option in a mixed window.
Pane/tab close dialogs remain **Close / Cancel**.

Cancel is still the initial selection and Escape target. Close retains its
existing destructive behavior. Detach rechecks eligibility at activation,
clears the pending close request and uses the same keepTerminals shutdown
policy as daemonaction::quitKeep. The existing teardown then disables
endInDaemon for all panes, so no EndSession is sent for the detached programs.
No new detach transport or session lifecycle mechanism is introduced.
A stale/forged detach action cannot shut down a mixed local/daemon window.

## Verification

- Pure model/dialog tests cover empty/ended/live/mixed eligibility, exact
  button actions, Cancel default, keyboard Detach, Escape and unchanged pane/tab
  choices.
- New window_detach_native test starts a real owned daemon and two native
  windows sequentially using the existing layout-smoke infrastructure. The
  first window runs sleep in two tabs, verifies Cancel keeps both session IDs,
  then detaches. The second reattaches both IDs, verifies replay markers and
  confirms the programs are still live/busy rather than merely stale output.
- Existing settings_native local and tunneled runs now verify the actual local
  Settings PtyLink prevents window detach, including rejecting a stale action
  without ending the local process or closing the window.
- Full native build is warning-free; **146/146 tests passed**, 94.13 seconds,
  excluding only the two older layout scripts whose cleanup uses pkill -f.
  Test cleanup uses owned child PIDs, temporary sockets and directories.
- Changed C++ formatted with git clang-format. Local clang-tidy remains
  unavailable (run-clang-tidy missing); integrator should run focused tidy.
- No physical desktop click/visual check was performed. Native smoke uses SDL's
  dummy backend and the application's existing action/dialog paths.
