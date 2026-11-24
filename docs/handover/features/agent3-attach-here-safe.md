# Safe Attach terminal here and unused-pane bypass

Scope: same-pane Attach terminal here only; no scheduler/transport/backpressure
change. This does not refactor every other attach command.

Commits (selective integration, no merge of this old-history branch):

- `30e8ee4` tests-first, `c82ba87` stable-ID ownership exchange API.
- `c64433e` tests-first, `6b1f6f4` staged replacement / unused split bypass.
- `afde6a1` real SDL Escape cancellation regression.

All these commits use the required human author identity. No main sync/merge
has been performed after the attribution rewrite.

## Unused-pane bypass

Reuse ShellFacts/isEmptyShell, with an explicit pane scope that does not require
its tab to contain one pane. The other guards are unchanged: no input ever sent
(including typed then deleted text), known foreground probe, not busy, zero
scrollback, no selection/copy mode. Unknown metadata is not an unused shell.
The Attach Here selection still rejects the own/current terminal and terminals
already shown in this window. Attached-to-another-client targets require the
existing takeover confirmation before bypass. A used pane retains Replace and
end / Replace and keep / Cancel; the coordinator's default End/Esc-Cancel stays.

## Transactional replacement

The former swapTerminal detached/ended the old terminal before attempting the
new attachment. It is removed. A staged NativePane owns the candidate view and
connection independently. Its endInDaemon is false from creation, so cancelling
or destroying staging NEVER ends the target program.

The original pane remains in the workspace with its model, selection, session,
focus/ID and split geometry until the candidate is Running, live, and its
acknowledged ID matches the requested ID. While waiting, ordinary original-pane
pumping/input/query replies continue. Staging is also pumped in the same GUI
thread; no threads or transport budget changes are added.

Immediately before commit, recheck original object identity, PaneId/session ID
and liveness, candidate state and deadline. On success, replacePaneAnywhere
atomically transfers the candidate owner into the SAME leaf ID and returns the
old owner. Only AFTER the workspace contains the verified replacement is the
old owner retired, with its endInDaemon set to the explicit End/Keep choice.
This avoids dangling parser callbacks: each TerminalView stays at the same
object address as its own DaemonLink callbacks across the unique_ptr move.

Errors, a missing target, timeout (five seconds), stale/replaced/closed original,
or candidate loss/takeover abort staging. Escape cancels a focused pane's pending
attach before its next pump when no overlay owns the key. Candidate shutdown
drops only that connection, never sends Close/EndSession. The original remains
unless another explicit action closed/replaced it. A replaced original is never
accessed through a stale pointer; the saved pointer is compared only for identity.

Pending staging is bounded to one entry per PaneId; a newer request cancels its
previous stage. Stage candidates do not count as visible splits/tabs. Terminal
query parsing and wire order are unchanged. Metadata is rechecked before an
explicit replacement to preserve takeover confirmation, including forced-choice
propagation. This is not an atomic server-side lease precondition; normal
network races after confirmation remain governed by the existing protocol.

## Verification

- Pure policy tests: waiting/running/live/ID/identity conditions, old/target loss,
  failure/cancel/deadline, conservative known/unknown/typed/busy shell guards.
- Workspace tests: atomic owner handoff, missing/null non-consumption, stable
  ID/split/focus/tab, hidden and parked targets; old owner remains caller-owned.
- Ten real-daemon native smoke cases: untouched split bypass; typed+deleted and
  busy prompt preservation; unknown probe injection; missing-target handshake
  failure with selection preserved; actual SDL Escape cancel; injected deadline;
  original owner replaced under the same ID; original pane closed; attached-peer
  takeover confirmation. Check no synchronous old destruction, original/target
  survival on failure, successful replay/live new owner and old removal only
  after commit. Other split remains and the split count/focus/PaneId are stable.
- Full warning-free build: **150/150 tests pass**, 135.28 seconds, excluding only
  the two old prohibited-pkill layout scripts. Changed C++ formatted.
- ASan+UBSan Debug **no-PIE**: all ten real-daemon attach cases and four Settings
  local/tunneled repeated-open/close cases pass with no sanitizer report.
- Settings user whole-window crash/slow-start remains unreproduced; phase-only
  measurements and strengthened lifecycle tests are not a claimed fix.
- No real desktop GPU/physical terminal or actual remote SSH workload check.
  Tests use SDL dummy/software and owned daemons/config/sockets/PID cleanup.
