# Cell Colors and Palette Contract

The approved layout remains exactly 12 bytes, naturally aligned:

| Field | Offset | Bytes |
|---|---:|---:|
| codePoint (char32_t) | 0 | 4 |
| foreground payload (Rgb8) | 4 | 3 |
| background payload (Rgb8) | 7 | 3 |
| attributes/kinds (uint16_t via CellAttribute) | 10 | 2 |

Compile-time assertions verify sizeof, offsetof, standard layout, and trivial
copyability. No packed structs, pointers, or allocations are added to each cell.

Bits 0..7 are style flags; bits 8..9 encode foreground kind; bits 10..11 encode
background kind; bits 12..15 remain reserved. Kind 0 is literal RGB (preserving
existing aggregate initializers), 1 is indexed (index in payload.r), 2 is default
(resolve according to foreground/background role), and 3 is reserved (currently
falls back to the role's default). ColorReference is an unpacked transient API
value; it is not stored as an extra field in FChar.

Use FChar.foregroundColor/backgroundColor and setForeground/setBackground instead
of interpreting tagged payloads as RGB. setStyle/style exclude color-kind bits.
Direct assignment to the public attributes word overwrites kinds too; new code
should use the style helpers. Whole-cell copies retain all bits.

VtParser stores logical default/indexed/RGB identities. SGR 0/39/49 restore the
parser's configured defaults, which are symbolic theme defaults for the ordinary
constructor. The legacy four-argument RGB constructor intentionally selects
literal defaults. Direct TerminalModel.setColors and existing FChar RGB aggregate
initializers also remain literal; use setColorReferences to opt into theme colors.
Erased blanks retain color references, not text-decoration flags.

Palette owns 256 configurable RGB entries and role-specific defaults. Its built-in
16 colors are Tango/GNOME-inspired; defaults are light text on #1e1e1e, not an exact
match guarantee for a particular GNOME release. Indices 16..255 use the conventional
6x6x6 cube and grayscale ramp. Palette is ordinary caller-owned data with no file,
GUI, parser, or SDL dependency.

RenderFrame borrows an optional Palette pointer during render; null selects the
SDL backend's dark default. Resolve colors, then apply inverse, conceal, and faint.
Changing a palette updates already stored indexed/default cells on the next frame;
RGB cells remain unchanged. It does not mutate terminal text or atlas resources.
The native PtySession now uses symbolic defaults, so the dark theme applies to
new output and erased cells. Existing direct demos retain their literal artwork.

This is the stable contract for independent settings/benchmark tasks. Persistent
configuration and a GUI are not implemented; see SETTINGS_AND_PERFORMANCE.md.
OSC palette changes and alternate theme ownership are future policy decisions.
