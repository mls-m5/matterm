# Three-line terminal list (frontend stage)

Contract: `docs/tasks/matmux-rich-list.md`, main base `3ae5b94`.

Each entry, including New terminal, occupies a three-line block:

1. Persistent `>` on the selected block, digit shortcut, name/folder basename,
   own/attached warning and foreground program.
2. Width-clipped folder, foreground-program status, attached status and recent
   output age when known. A foreground shell is **not** described as an idle
   prompt. Output age is a metadata snapshot, not a last-command timestamp.
3. Bounded recent-output preview, or explicit unavailable/disabled/empty status.

Selection colours cover every cell of all three lines. The stored colours are
reversed before setting tty's inverse flag, so the effective selection remains
light rather than accidentally turning dark. Tests emit the shared Canvas's
SGR into the existing VtParser/TerminalModel and resolve the final Palette colours
for both truecolour and 16-colour fallback. The literal marker survives discarding
all attributes. No custom terminal parser is introduced.

Scrolling, digits and page sizes count entries, not visual lines. On small
screens header/blank/footer space is dropped first; below three usable lines the
selected block is clipped without hiding its name/marker. End confirmation,
self-attach/self-end protections and takeover warnings remain. Refreshing the
list or navigating it never attaches a terminal to obtain a preview.

The frontend model has optional `foregroundBusy`, `outputAgeMs`, and `preview`.
It can disable previews independently of layout. Protocol/settings mapping is
not implemented in this stage; agent2 owns the approved 1.3.5 metadata source and
`terminalListPreview` setting. Existing daemons show unknown preview/age.

`ttyscreen.h::put` now bounds decoding to 4096 input bytes and uses the existing
tty decoder and width table, rather than counting codepoints as columns. It
clips without splitting wide characters and renders C0/C1/bidi controls inert.
Preview validation is stricter: any control-bearing preview is omitted entirely,
not ESC-stripped (which could disclose an OSC/DCS payload). The backend must
supply plain text using the existing VT lexer; frontend checks are defense in
depth, not a substitute for that parser. Canvas has no combining clusters, so
zero-width marks are skipped, consistently with the settings TUI.

## Verification

- Tests-first commits for block selection/navigation/metadata safety and for
  effective SGR colours/control-payload rejection.
- Full native build: no warnings.
- `ctest --test-dir build-daemon -j4 --timeout 180 -E
  '^layout_smoke_(local|remote)$'`: **142/142 passed**, 99.9 seconds. Includes
  tty_list, tty_attach, shared tty/frontend regression tests, settings, complete
  real-daemon matmux_pty, end/own protections and fresh-start terminal queries.
- The two older layout scripts are excluded because their cleanup uses forbidden
  `pkill -f`; the existing native diagnostics test exercises the built-in layout
  smoke locally/remotely with owned-process cleanup.
- clang-tidy cannot run: `run-clang-tidy` is missing.
- No actual phone/physical terminal or human visual colour/monochrome check yet.
  No backend metadata or live privacy-switch validation until mapping is wired.
