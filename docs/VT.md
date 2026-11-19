# Initial UTF-8 / VT Subset

`matterm::vt` provides `VtParser`, independently of SDL, a PTY, or either demo.
It borrows a `TerminalModel`. Feed arbitrary byte chunks; chunk boundaries have
no semantic meaning. Call `finish()` at EOF. The same model remains usable via
its direct API when no parser is attached.

```cpp
TerminalModel model(80, 24, 1000);
VtParser parser(model, {
    [](std::string_view reply) { /* copy into an outbound transport queue */ },
    [](std::string_view title) { /* optionally update a GUI title */ }
});
parser.feed("\x1b[31mHello, ");
parser.feed("world\x1b[0m\r\n");
parser.finish();
```

Callbacks receive borrowed views valid only during the call and must not re-enter
the parser. Copy data before retaining it. Parser-created callbacks never execute
OS commands, read/write files, access the clipboard, or open URLs. While attached,
the parser owns rendition/mode state; do not independently change model colors or
screen mode. Resizing, viewport navigation, rendering, and blink timing remain
client responsibilities. Consume model frame views before feeding more bytes.

## Supported

All escape sequences below use seven-bit `ESC` introducers, not raw C1 bytes.
CSI parameter coordinates are one-based; omitted/zero movement parameters use one.
Movement clamps to the screen or origin-mode margins. Resize currently resets
margins/origin to the full screen; applications can re-establish them on SIGWINCH.

| Input | Behavior |
|---|---|
| UTF-8 text | Incremental decoding; invalid input becomes U+FFFD; one code point per cell |
| CR / LF / BS / HT | Existing model operations; LF preserves column; tab stops every eight columns |
| `ESC D`, `ESC E`, `ESC M` | Index / next line / reverse index |
| `ESC c` | Reset model/buffers/rendition/input modes (RIS) |
| `ESC ( B/0`, `ESC ) B/0`, SI/SO | ASCII or DEC line graphics in G0/G1 |
| `CSI A B C D` | Relative cursor up/down/right/left |
| `CSI E F` | Relative line movement and column zero |
| `CSI G`, `CSI d`, `CSI H` / `f` | Column, row, or row/column positioning |
| `CSI a`, `CSI e` | Relative right/down aliases |
| `CSI J`, `CSI K` | Display/line erase modes 0, 1, 2 using current colors; history is retained |
| `CSI @`, `CSI P`, `CSI X` | Insert/delete/erase characters |
| `CSI L`, `CSI M` | Insert/delete lines within the cursor-to-bottom margin |
| `CSI S`, `CSI T`, `CSI r` | Scroll region up/down and set inclusive margins |
| `CSI 4 h/l`, `CSI ? 6 h/l`, `CSI ? 7 h/l` | Insert, origin, and autowrap modes |
| `ESC H`, `CSI g`, `CSI I/Z` | Set/clear tab stops, forward/backward tabs |
| `CSI Ps SP q`, `CSI ? 12 h/l` | Cursor shape/steady/blinking style and blink mode |
| `CSI m` | SGR subset below |
| `ESC 7` / `8`, `CSI s` / `u` | Shared per-screen saved cursor/rendition slot, including pending wrap |
| `CSI ? 1 h/l`, `CSI ? 2004 h/l` | Application cursor and bracketed paste input modes |
| `CSI ? 25 h/l` | Cursor visibility without clearing pending wrap |
| `CSI ? 1049 h/l` | Initial alternate-screen policy below |
| `CSI 5 n`, `CSI 6 n` | Status/cursor reply; cursor row is origin-relative when enabled |
| `CSI c`, `ESC Z` | Conservative primary DA reply `CSI ? 1;0 c` (VT100 class, no advanced options) |
| `OSC 0;title`, `OSC 2;title` | Optional title event, terminated by BEL or `ESC \` |

### Color SGR

- 0 or empty: reset foreground/background to constructor defaults.
- 30-37 / 40-47: normal palette foreground/background.
- 90-97 / 100-107: bright palette foreground/background.
- 39 / 49: default foreground/background.
- 7 / 27: inverse on/off; 8 / 28: conceal on/off. Logical colors are retained,
  while effective cell colors are resolved before storage.
- `38;5;n` / `48;5;n`: 256-color index (0-255).
- `38;2;r;g;b` / `48;2;r;g;b`: RGB components (0-255).

Indices use a conventional xterm-style 16-color palette, 6x6x6 color cube, and
grayscale ramp. Defaults are independently configurable. Invalid known color
groups are consumed without changing that color. An unknown extended-color format
stops the remaining SGR parameters to avoid reinterpreting its payload.

SGR 1/2/3/4/9 enable bold/faint/italic/underline/strike; 22/23/24/29 disable
them. SGR 5/6 enable a shared blink attribute and 25 disables it. Bold/italic
use separate atlas entries; underline/strike are geometry, faint blends the ink
with the background, and RenderFrame.textBlinkVisible is driven by the caller.
Double underline and other unsupported attributes are ignored. Colon-separated SGR is unsupported and the entire CSI is discarded.

### Initial Alternate-Screen Policy

Only private mode 1049 is recognized. Entry preserves the primary screen's cells,
history, cursor, rendition, pending wrap, viewport, and save slot; the alternate
screen is cleared/homed and initially inherits current colors. Alternate output
has no scrollback. Repeated entry/exit for the active mode is a no-op. Exit restores
the untouched primary state. This deliberately reuses the existing direct model
policy; it is not a claim of complete xterm DEC save/restore semantics.

Modes 47, 1047, and 1048 are ignored pending separate implementations and tests.
Cursor save/restore currently includes the model's CursorState, including visibility.
The future compatibility layer may need finer-grained DEC/ANSI state handling.

## Bounds and Recovery

- CSI uses a fixed array of at most 32 parameters, with each value at most 1,000,000.
  Excess parameters, overflow, malformed parameter syntax, and unsupported
  intermediates cause the sequence to be ignored through its final byte.
- OSC storage is capped at 2,048 bytes including selector. Oversized OSC is
  discarded until its terminator, not applied as a truncated title.
- Unsupported DCS/SOS/PM/APC strings are discarded without storing payload, until
  `ESC \`. BEL terminates OSC only. Clipboard OSC 52 and hyperlink OSC 8 are ignored.
- CAN/SUB cancel an in-progress sequence. ESC restarts an escape/CSI sequence.
  Inside control strings, malformed embedded escapes make the string non-actionable
  and it remains discarded until BEL/ST or cancellation.
- Unknown escape/CSI commands are consumed without painting their bytes. ASCII
  and DEC line-graphics G0/G1 designators are supported; other charsets are ignored.
- `finish()` replaces incomplete text UTF-8, discards incomplete control sequences,
  and retains completed rendition/model state. `resetInput()` drops pending input
  without inserting a replacement and also retains completed state.
- Raw eight-bit C1 control forms, secondary/extended device attributes, erase
  history mode 3, application keypad, and mouse reporting are
  not supported yet. Primary DA does not imply full VT100 conformance.
Do not advertise full VT100/xterm compatibility or Vim support.

## Applications and Next Steps

Both existing demos remain unchanged. The separate `matterm` application is an
native terminal on Linux: a bounded PtySession feeds the parser, and replies join
the same outbound queue as keyboard input. See PTY.md for lifecycle and input
semantics. `--showcase` and `--input FILE` retain transport-free VT/file viewing;
file mode reads at most 4 KiB per GUI frame and does not send replies anywhere.
OSC titles only update the GUI title. Both original demos retain their behavior.

Tests compare complete input against every split point and byte-at-a-time feeds,
check model state/colors/events, and exercise malformed/oversized/truncated input.
The application smoke test also renders fragmented VT input and verifies primary
pixel restoration after alternate-screen output. These are an initial suite,
not comprehensive terminal conformance tests.

Reference: https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
