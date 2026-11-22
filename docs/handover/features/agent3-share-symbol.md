# Share button symbol

Written by agent3, 2026-11-22.

## What changed

The Share button in the tab strip shows `∴` (U+2234, three dots; looks like the share icon of a phone)
instead of the word "Share". Nothing shared in the active tab: grey `∴`. N panes shared: red `∴ N`.
The colour is not the only difference (the owner is colour blind): shared adds the count to the text.
Clicking works as before (`share.tab` / `share.tab.stop`); the button is only offered when the
panes run in the local daemon. The button is one column narrower than " Share " (3 columns idle).

## How to check

Start `matterm` (daemon on): the strip ends in ` ∴ `. Press it: the focused tab's panes are shared and
it reads ` ∴ 1 ` (or more) on a red background. Press again: back to grey ` ∴ `.

## Changing the symbol

One constant: `kShareIcon` in `src/frontend/tabstrip.h` (it is in `kStripGlyphs`, so the glyph test
checks the bundled font has it). Other candidates the font has: `↗` U+2197, `⇧` U+21E7.

## Verified

Tests: `test/tabstrip_test.cpp` (the label text for 0, 2 and 12 shared, no digit when nothing is shared,
widths and layouts at 0..200 columns), `test/workspaceview_test.cpp` (clicks and actions), the font
glyph test (`chat`). ctest 89/89.

## Not verified

How `∴` looks on a real screen (size, legibility at small font sizes). The web client draws no Share
button, so it is unaffected. The strip's other text (palette and shortcut help) did not contain the word
"Share" for the button, so nothing else changed; the palette entries "Share this terminal" keep their words.
