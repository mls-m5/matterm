# Handoff: SDL_ttf zero-width rendering exit

## Scope and base

Worktree `.tmp/agent3`, branch `agent3-resize`, clean before starting. Updated
with `git merge --ff-only main` to base
`a20c8ad4c9810c8e898ee0e4dc1de2cd20a3a95b`. Only glyph atlas,
its tests, roadmap and handoff documentation are changed. No main checkout,
server, submodule or push changes.

## Reproduction

SDL_ttf 2.20.1, SDL2 2.26.5, installed DejaVu Sans Mono and DejaVu Sans.
A temporary standalone probe scanned provided BMP characters with nonempty glyph
metrics, then called `TTF_RenderGlyph32_Blended`. U+00AD (soft hyphen) produced
`Text has zero width` in both fonts at sizes 1, 2, 6, 12, 18 and 24. No other
characters failed in this particular scan. No user logs were required.

At size 24 in DejaVu Sans Mono, U+00AD has metrics x=4..11, y=6..8 and advance=14,
so the atlas's existing empty-glyph-metrics guard does not catch it. However,
`TTF_SizeUTF8(font, "\xc2\xad", ...)` reports text width zero and rendering
fails. Synthetic bold/italic styles may add surface padding, so measurement
must use the currently selected style rather than assuming one width per code
point or simply suppressing every soft hyphen.

The new `Zero-width rendered text is cached without allocating an atlas slot`
unit test was run against the old atlas before the fix. It failed with
`crashed!` and `error: Text has zero width` (8 other atlas cases passed).

## Fix and API

`src/render/glyphatlas.cpp`: after selecting the actual primary/fallback font,
applying bold/italic, and obtaining glyph metrics, measure the scalar's UTF-8
text width. On successful measurement with width zero, cache an empty glyph and
retain the advance. Otherwise use the existing rendering/upload path. There is
no exception catch or SDL error-string matching; failed measurement, failed
rendering, failed texture operations, full atlas and oversized glyph errors
still propagate. Measurement is on cache misses only, not each rendered cell.

No public API, model, selection or character-width policy changes. This is not
combining-mark shaping or soft-hyphen line-breaking support. The terminal still
uses its existing one-cell-per-scalar model; an empty bitmap does not delete the
cell or alter stored text.

Tests check regular/bold/italic soft hyphen, preserved advance, repeated cache
lookup, no shelf allocation for empty output, and subsequent visible insertion.
The existing full-atlas test now checks the actual full-atlas diagnostic; an
oversized-glyph test checks that its distinct diagnostic still propagates.

## Verification

```sh
cmake -S . -B build -DMATTERM_BUNDLE_FONT=OFF \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build -j4
ctest --test-dir build --output-on-failure

cmake -S . -B build/no-fontconfig -DMATTERM_BUNDLE_FONT=OFF \
  -DMATTERM_SYSTEM_FONT_FALLBACK=OFF \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build/no-fontconfig -j4
ctest --test-dir build/no-fontconfig --output-on-failure
```

Both configurations: **32/32 CTest cases passed**. The atlas executable now has
10 unit cases. A native PTY/dummy-renderer run also exited successfully after
printing synthetic soft-hyphen output:

```sh
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
  ./build/matterm /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
  -- /bin/sh -c "printf 'soft\302\255hyphen'; sleep 0.2"
```

## Remaining uncertainty

The exact input/font from the originally reported native session is unknown;
this proves a reproducible cause of the same fatal diagnostic, not that the
original session contained U+00AD. No new fallback-specific fixture was needed
to reproduce it. The guard is after fallback selection and applies equally to
fallback fonts, but a fallback-only zero-width case was not separately forced.
No physical-display, browser, bundled-JetBrains-font or performance measurement
was performed. Other SDL_ttf versions/fonts may choose to render soft hyphen;
the test measures each style and does not assume that all versions suppress it.
