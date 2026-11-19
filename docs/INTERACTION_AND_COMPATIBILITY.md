# Interaction and Compatibility Follow-up

## Color and font reports

- **Shell prompt appears white.** matterm still defaults to `TERM=vt100`;
  that terminfo description does not advertise colors. Compare `TERM` and
  `tput colors` in matterm, GNOME Terminal, and inside/outside tmux. Test
  `matterm --term xterm-256color` before deciding on a new default or a dedicated
  terminfo entry. Check which prompt rule suppresses color; do not override
  the inner `TERM` value that tmux sets for its panes.
- **Pi code/tool panel backgrounds look like the default background.** Test
  `printf '\033[31mred\033[0m \033[48;5;22m green \033[0m\n'` in both
  terminals; also test `48;2` RGB. Both semicolon forms are already covered by
  parser tests. If these render correctly, capture a minimal VT stream from a
  failing panel and compare emitted codes/resets/erasures with a reference
  terminal. Colon-separated SGR is a known gap, not a confirmed cause.
  Regular layout works in matterm outside Podman, but both regular and
  fullscreen have shown the symptom inside Podman. Whether the same container
  works in GNOME Terminal is not yet confirmed. Both existing captures include
  `CSI ? 1049 h`; replay of the GNOME capture preserved the green RGB
  background in matterm. Literal RGB and `PI_TRUE_COLOR=1 pi` worked inside the
  affected container. The native application now advertises `COLORTERM=truecolor`
  without changing TERM. If Podman does not pass it through, use `--env COLORTERM`.
  The failing session was subsequently confirmed to have `COLORTERM=truecolor`
  in Pi's process environment, so the hint alone is insufficient. Capture that
  panel using matterm's `--record-output FILE` (see [PTY.md](PTY.md)), without
  inserting another PTY via `script`.
- **Braille spinner becomes a replacement glyph.** JetBrains Mono lacks these
  code points; they are three-byte UTF-8, not clusters. Verify Linux Fontconfig
  fallback with the default font and an installed face that covers Braille
  (see [FONTS.md](FONTS.md)). General shaping and wide/combined cells remain
  separate. Record the actual font picked when investigating coverage.
- **Font sizes differ from GNOME Terminal.** matterm uses render-target pixels;
  see [FONTS.md](FONTS.md). Desktop/HiDPI-aware sizing is a separate feature.

## Selection and terminal mouse input

Local text selection and mouse reporting to a running application are distinct.
Keep left-button drag and Copy available when applications request mouse input,
with a local modifier override (proposed: Shift). Add VT mouse tracking (press,
release, wheel, drag, optional motion), modifier bits, and SGR coordinate encoding
before declaring compatibility with tmux and mouse-enabled TUIs. Test those
modes, alternate screen, and their interaction with the local context menu.
Local right-click Copy/Paste, word selection, and auto-scroll during drag can be
added separately.

## Window chrome

SDL2 provides [window hit testing](https://wiki.libsdl.org/SDL2/SDL_SetWindowHitTest),
not a GTK-style header bar widget. Plan a future borderless SDL window with a
custom header bar. SDL hit testing will reserve drag and resize zones; buttons
and terminal cells will remain normal input zones.

Treat this as substantial GUI design work, not just a window flag. Prototype the
layout and visual hierarchy first: title, window controls, spacing, typography,
icons, and active/inactive states. Design hover, pressed, keyboard-focus, and
accessible controls, and check contrast and HiDPI sizing. Then verify resize,
maximize, restore, and fullscreen behavior without disturbing terminal geometry
or mouse selection.

Keep native decorations as the default during rollout and as a fallback when
custom decorations are unsupported. Test X11 and Wayland separately before
considering a change to the default.
