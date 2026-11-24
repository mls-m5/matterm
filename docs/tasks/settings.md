# Settings: a settings file, themes per computer, and a settings TUI

Owner's wishes (2026-11-23) and decisions (do not reopen; ask with alternatives if something is unclear)
- **Settings live in one JSON file** `$XDG_CONFIG_HOME/matterm/settings.json` (else
  `~/.config/matterm/settings.json`), strictly read with the existing strict JSON reader (no new
  dependency), written atomically (temp file + rename). Unknown keys are kept when the file is
  written; a broken file is reported and the old values stay.
- **The window follows the file**: it checks the file's modification time about once a second
  (one `stat`, no performance cost; stays cheap) and reloads when it changed; a palette entry
  "Reload settings" does it at once. inotify can be added later; polling stays as the baseline.
  The settings file belongs to the computer the window runs on (the client), also with `--daemon-host`.
- **The settings program is a text UI ("a fancy TUI"), a program of its own**: `matterm --settings`
  and `matmux --settings` start it in the terminal you are in; the palette entry "Settings"
  opens it in a new tab (a pane that runs it). It uses the shared `src/tty` layer; no dependencies.
  A mockup of the look: `.tmp/settings-mockup.sh` (the owner has seen it; ask the integrator for
  the approved changes before building the screens). Search at the top matches as permissively as
  the command palette (`searchCommands`); a list of settings with the value and a short text.
- **Themes**: only dark ones for now: Grey (today's look), Blue, Green, Red (and maybe Amber).
  A theme colours the frame: the tab strip, marks, borders and edges, plus a slight tint of the
  terminal's default background (text contrast kept; programs' own colours are not touched).
  The owner has reduced colour vision, so colour is not the only sign: for terminals on another
  computer the **computer's name is written in the draggable gap of the tab strip** (between the last
  tab and the share button), in a muted colour of the theme, **not in the tab's name**.
  The themes differ in hue only (no variation of brightness for now): stylish and simple first.
  The look stays calm.
- **Theme per computer**: the window picks the theme from the computer it is connected to:
  `--daemon-host` name -> a theme, **automatic** by default (the name picks a theme in a fixed
  order, so the same computer always gets the same one) with a per-computer override in the
  settings ("server -> Green", or back to automatic). The local computer keeps the Grey theme
  (or the user's default theme). Keep the implementation simple. Detecting a plain `ssh host` typed inside a shell is later, not now.
- First settings in the list: Theme (default), Theme per computer, and the existing flags worth
  turning into settings (tab activity icon/pulse/off, share symbol is fixed, window frame
  own/system, scrollback rows if it is cheap, confirm before closing a busy pane). Keep it small.

## Split
- agent2: the settings model (`src/frontend/settings.h`, a list of definitions with key, text,
  type, default, allowed values), reading and atomic writing of the JSON file, the change watcher
  (mtime poll, pure and testable with a fake clock/filesystem), the theme data (palette + accent +
  tint, derived `Colors` for the strip and the border colour), the per-computer rule (automatic
  choice, overrides), and wiring the window to it (reload, theme per daemon host, the host name
  as muted text in the strip's draggable gap). Tests first. Palette entry "Reload settings".
- agent3 (after the mockup is approved): the settings TUI (screens as in the mockup, search,
  editing values, live theme preview), `--settings` for `matterm` and `matmux`, the palette
  entry "Settings" opening the tab, completion, README.
- Docs: README, TABS_PLAN (theme section), a feature description per agent in
  `docs/handover/features/agentN-<name>.md`. Rules as before: tests first, small commits, tidy
  clean, SHAs to the integrator; state what was not run (no display here).

## TUI continuation contract (2026-11-23)

Agent2 now owns the settings TUI; agent3 finishes client performance logging.
Use two columns (sections left, controls right), centered on wide terminals.
Reuse src/tty Session/Canvas/KeyDecoder and searchCommands; no new widget/input
framework and no separate preview panel. Applying a control saves immediately
through the existing atomic writer; the window follows within one second.
Escape cancels only the current edit, never earlier saves. Re-read before
applying a single-field edit, preserve unknown keys/other external edits, and
refuse a conflict on the edited value or a broken file. A signature check just
before save catches intervening external changes; arbitrary non-cooperating
writers can still race the final check/rename (not a cross-process CAS).
`matterm --settings` / `matmux --settings` edit the local client's file without
starting a daemon/SSH/window. Palette Settings creates a new LOCAL process/tab,
also when the window uses --daemon-host. Shared CLI/ptyfrontend/CMake changes
wait for the integrator's combined performance base; agent3 has released them.
New files: src/frontend/settingsui.h, app/settingsrun.{h,cpp}, settings UI tests.

## Addendum (owner, 2026-11-23): where the computer's name sits
The name goes roughly in the **middle** of the strip's draggable gap. Make the placement a setting
to try out: `computerNamePlacement` = `center` (default) | `left` (next to the tabs) | `right`
(against the buttons), a choice control in the settings, applied live like the theme.
`placeComputerName` in `tabstrip.h` stays the one place. (agent2, small.)

## Addendum 2 (owner, 2026-11-23): the file
The file is rewritten whole each time it is saved, always with the same formatting (indented, one setting per line, members in the order of the definitions). Settings that were missing are added with their default value, so reading the file explains what can be changed; unknown members are kept (after the known ones). No comments (JSON has none; the key names must be self-explanatory, and the settings program and a short docs page list what each one does and which values are allowed). agent2: change `Settings::toText`/`save` accordingly with tests (formatting is stable; a second save changes nothing).
