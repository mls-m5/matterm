# Recover tab and split layouts after an unexpected window exit

Owner request: automatically save tab/split layouts when they change (including
tab names), so that a window crash or lost connection does not leave only
individually attachable daemon terminals. Restore their existing daemon session
IDs and layout, not new shells in home. Distinguish this from named saved
sessions, which capture cwd/commands and may offer to start new programs.

## First implementation (named tabs)

Named tabs with only daemon-backed panes are checkpointed every second, without
RPCs, screen text or command capture. Atomic SessionFiles snapshots live in a
host/daemon-scoped recovery subdirectory. Each window holds a unique flock;
only checkpoints whose window lock is released are offered by Attach tab.
SIGKILL/crashes preserve the last checkpoint; SIGHUP/SIGTERM retain it and live
PTYs. Explicit ordinary shutdown retires its own checkpoints, while intentional
Detach continues to use the existing detached layout store. Restore refuses
missing/busy terminals rather than creating shells or deliberately taking over.
No automatic startup attachment is performed.

Tests cover SIGKILL lock release, live-window hiding, host separation, unchanged
write suppression, retirement, and a real daemon + native headless window:
SIGKILL the window and restore its named split with both surviving terminal IDs.
Real SSH/display verification remains necessary. The checkpoint interval can
lose up to one second of layout changes. Unnamed/mixed/local tabs are excluded;
SSH aliases spelling the same host differently remain separate recovery scopes.

## Design requirements

- Inspect existing detached-tab layout files, session IDs, workspace capture,
  daemon host identity and session policies; reuse where safe. Do not create a
  second layout serialization format without explaining why.
- Capture changes to splits, tab order/names and attach/detach; debounce and
  use atomic, private writes. Avoid per-frame writes and do not save terminal
  content, input or secrets. Never overwrite the last good file on parse/write
  failure. Avoid synchronous SSH RPC per pane during a layout update.
- Define ownership for multiple windows/hosts and avoid overwriting or stealing
  another window's layout. On crash, attach only the intended daemon sessions;
  do not automatically take over terminals currently attached elsewhere.
- Handle stale/missing IDs and old daemons gracefully; never start saved commands
  automatically. Explicitly distinguish controlled Close (end), Detach (keep),
  crash and reconnect, and remove/retire recoveries only at the right time.
- Propose discoverable recovery UX (e.g. palette action/list with timestamps and
  explicit confirmation), and ask the owner before choosing automatic startup
  reattachment or takeover. Keep named saved-session semantics separate.
- Include test-first fake storage and real-daemon test: create split layout with
  running programs, kill only the window without graceful shutdown, open another
  window, recover exact surviving session IDs/layout without terminating or
  restarting them; also multi-window isolation and controlled-close cases.

Coordinate with agent3's current settings crash and tab/pane detach work; avoid
concurrent edits to app/ptyfrontend.cpp. Document limitations and require real
SSH/display verification where sandbox tests cannot cover them.
