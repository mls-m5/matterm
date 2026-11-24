# Pane and tab close-confirmation Detach

Tests-first `1462b79`; implementation/native regression `a8417cd`.
The prerequisite `ca61ca5` adds Settings palette/multisplit reproduction steps;
`a8417cd` also strengthens that lifecycle test (not a claimed crash fix).

The existing confirmation now offers Close / Detach / Cancel for Pane, Tab or
Window when all live panes IN THAT REQUEST have identified daemon links and
no local PtyLink. Missing targets and all-ended targets do not qualify. Local
Settings cannot be detached; it prevents tab/window detach when in scope, but
an unrelated local tab does not prevent detaching a daemon-only target tab.
Cancel is the default and Esc answer for all three-button dialogs.

At activation the original pending scope/PaneId/TabId is rechecked. Pane detach
focuses that exact target and dispatches the existing daemon detach-terminal
action. Tab detach selects the original target and dispatches the existing
save-layout/detach-tab action. Window detach dispatches quitKeep. No new detach
protocol or EndSession path is added, and other tabs remain in the workspace.
Failure to save a tab layout retains the tab under the existing action policy.

Real-daemon native smoke independently tests Window, Pane and Tab, cancelling
before detaching, changing focus before confirmation, checking unrelated tab
preservation and unchanged live daemon-session counts, then launching a new
native window to reattach retained programs with replay and live/busy checks.
Tab reattachment verifies the saved two-split layout. Existing local and SSH
Settings tests check real local PTY ineligibility and unrelated daemon-tab
eligibility. Test cleanup uses owned child PIDs and temporary sockets/config.

Full warning-free build with phase diagnostics: 148/148 tests pass, 122.76s,
excluding only the two old prohibited-pkill layout scripts. No physical desktop
visual/click test performed. Changed C++ formatted; local clang-tidy unavailable.
