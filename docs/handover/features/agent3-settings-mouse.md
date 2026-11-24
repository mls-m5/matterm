# Settings panel mouse controls

Tests-first `c3e7e39`, implementation/tests `799d54e`.

`app/settingsrun.cpp` requests the existing tty ScreenOptions::Mouse::Click
mode, including SGR coordinates. Session cleanup restores mouse reporting on
normal exit and SIGTERM, alongside raw mode and alternate screen cleanup.
There is no new input protocol or native event decoder.

SettingsUI computes one Layout during draw: centered bounds, narrow section
width, vertical positioning, visible count and first scrolled row. Mouse hit
testing consumes that exact rendered layout. Resize is reflected by the next
paint; undersized terminals suspend all edit/apply input except exit.

Only left-button presses are actionable. Release, motion, other buttons, blank
space and outside clicks do not save. Section clicks select their section;
search clicks focus the existing search input; setting-row clicks select and
open that control. During editing only the explicit `[Apply]` and `[Cancel]`
buttons are actionable. Enter/Esc and all existing keyboard editing remain.
For new computer names, Apply advances to the choice stage just like Enter;
it does not silently skip validation. Choices still use keyboard arrows.
Mouse-wheel navigation is deliberately not added.

Verification:

- Settings UI tests cover centered and minimum-width layouts, scrolled rows,
  resize, section/search focus, explicit apply/cancel, and release/motion/right/
  outside suppression. Existing keyboard, concurrency and tiny-screen tests pass.
- Real PTY tests exercise SGR click input in both matterm and matmux settings,
  reject release/motion/outside applies, save via explicit click, retain keyboard
  cancel/apply, and verify mouse-mode enter/restore including SIGTERM.
- Full native build, no warnings; 145/145 tests pass in 124.34 seconds, excluding
  only the two legacy layout scripts with prohibited pkill cleanup.
- Native SDL physical mouse-to-settings-panel interaction was not exercised;
  this patch operates in the existing settings TUI and shared tty decoder.
- clang-tidy remains unavailable locally (`run-clang-tidy` missing).
