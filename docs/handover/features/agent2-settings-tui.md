# Settings TUI continuation (agent2)

## Scope and ownership

Agent2 owns the TUI and its tests; agent3 has released the shared CLI,
completion, native frontend and CMake files after client performance work.
The integrator selected combined main b25fb867586045276c7922ff0b6e28add4505320,
merged via b9512ba before edits to those shared files. Both squash conflicts were
resolved explicitly: retain matmux --perf-log and the new stale-save regression.
JSON reader migration and matperf integration are not part of this task.

`src/frontend/settingsui.h` is a settings-specific controller/layout, not a
new widget framework. It uses the existing settings definitions, watcher,
validation and writer; `searchCommands` supplies palette-equivalent fuzzy
search. `app/settingsrun.cpp` uses the shared tty Session, Canvas and KeyDecoder
for screen differences, capability detection, raw mode, resize and signal
restoration. No renderer, input parser, file format or dependency is added.

## Behavior

Two columns: Appearance / Terminal / Computers on the left, controls on the
right. The view is centered on wide/tall terminals and clipped to the available
size; very small terminals show a resize/quit hint. The selected control has a
value, explanation and restart/new-terminal note where applicable.

- Type to search across all sections; Backspace edits the query.
- Tab switches sections/controls. Arrows select; Enter edits.
- Choice/bool controls use arrows (Home/End also work), then Enter applies.
- Integer controls accept typed text, validated by Settings::set.
- Computers lists known overrides; Add computer asks for a host name and then
  a theme or auto. Name placement offers center/left/right.
- Applying a control writes atomically at once, with no Save button. The native
  window's existing watcher follows within about one second. Theme choice is
  reflected in the TUI itself; no separate preview panel is introduced.
- Escape cancels only the current edit. Earlier saves remain saved. Outside an
  edit it clears search, then exits. Ctrl+Q / Ctrl+C exits directly.
- Validation and disk errors remain visible and keep the edit open.
- Below 42x14, edits/applies are suspended; quit/cancel still work and resizing
  back retains an unfinished edit. Choice values reserve space before long
  labels. Wide-character names are placed at terminal-cell widths (combining
  clusters remain a shared Canvas limitation).

Both CLI modes dispatch before daemon/SSH/SDL initialization and reject other
command modes (--new, --name, native smoke modes). Native palette Settings
starts `/proc/self/exe --settings` through a local PtyLink, deliberately bypassing
the daemon factory. The one-shot override is consumed before anything can
throw, so a failed launch cannot redirect later ordinary tabs or splits.

Before each apply, the controller explicitly reloads the file, verifies that
the edited field did not change externally, and applies just that field to the
latest document. Thus external edits to other settings/unknown members survive.
A same-field conflict or broken JSON is refused, not overwritten. Watcher::save
also checks the file signature immediately before writing. This is conflict
avoidance, not an atomic CAS against arbitrary non-cooperating external writers:
there remains a narrow check/rename race. The loaded document is never discarded
just to save a stale whole form.

## Verification

The initial unit-test commit failed compilation without settingsui.h; the stale
save regression failed before the watcher guard. The real-PTY entry-point test
failed before dispatch was wired. An independent read-only review found hidden
choice values, invisible saves on tiny terminals, and skipped wide-name cells;
three regression cases were red before their fixes. Its integration pass found
a launch override left set after a factory exception, missing native mode
validation, and a weak Escape test; injected-failure and CLI regressions were
red before their fixes. Escape is now tested by cancelling and reopening an
edit, then applying a different choice, not merely quitting without saving.

Eight UI and fifteen watcher cases pass. settings_pty tests matterm and matmux
under a real PTY, with SDL deliberately unavailable and a non-connectable remote
host option: saves, cancellation, complete defaults, normal/SIGTERM terminal
restoration, non-TTY rejection and incompatible modes. settings_native runs the
existing hidden native smoke infrastructure with a real daemon and SDL dummy,
locally and via the existing fake SSH relay. It verifies a new local Settings
pane, no extra daemon terminal, the client settings file, watcher propagation,
and clearing the one-shot local override after an injected factory exception.
Changed-file clang-tidy is clean; the full native build is warning-free.

The broad native run passed 142/142, including matmux_pty, native_perf_log and
chatd_soak; only layout_smoke_local/remote were excluded because their existing
cleanup uses pkill -f. The final broad rerun after all review fixes also passed 142/142 in 90.45 s;
the targeted five-test integration rerun passed as well. No real display/GPU, SSH server or LAN was used; the
visual appearance has not been inspected on a real screen.
