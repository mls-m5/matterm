# Themed split gutters and thin terminal-area edges

Selective patch chain after Attach Here work: tests-first `b0a4705`, additional
SDL/fullscreen tests `4e899b6`, implementation `fd4e6c7`. Human commit identity
used; no merge/sync with rewritten main. The earlier experimental pump policy
is not a dependency of this feature.

## Follow-up: right-strip background regression

Tests `1eac145`, `81207ca`, `ecda9e1`; fix `c706b73`.

Actual SDL A/B readback against the pre-split paint order reproduces a change
at the rightmost viewport pixel in all 16 system-frame matrix cases (normal /
alternate screen, history present / absent, single / split pane, grey / blue).
The other eleven reserved-strip pixels are unchanged. The original custom
terminal background 0x3c5a78 became dark decoration 0x171717 with grey theme.
The same post-pane outline can replace the scrollbar's last pixel and overwrite
a full-block glyph at the left edge. The matrix uses real TerminalModel normal
and alternate frames, not a fake alternate-screen flag. Actual track/thumb and
wrap-marker pixels remain tested separately.

Root cause: the post-pane `SplitDecoration.outer` paint, not beginFrame,
PaneArea sizes, tint, or reserved-strip cell/background rendering. These were
not changed by the split patch. No wider-than-one-pixel right-strip regression
was reproduced; a user report of a wider rectangle needs separate evidence.

The fix removes ALL terminal-boundary overlays and their gutter clipping.
Only existing gutter pixels are decorated; their middle now naturally reaches
the themed tab strip without a dark top seam. No renderer, cells, layout,
scrollbar or hitbox changes. This deliberately leaves the requested additional
own-frame dark border pending: it must later use the EXISTING frame's innermost
pixel OUTSIDE the viewport, omit scrollbar edges and remain absent at screen
edges when maximized/fullscreen. System-frame windows must have no extra app
outline at all. The older outer-ring description below is superseded. Warning-free full build
and 151/151 tests pass in 129.32 seconds after this follow-up. The SDL readback
matrix passes, including original strip RGB, actual thumb/marker pixels and
full-block edge glyph preservation. Physical desktop/GPU appearance remains
unverified.

## Geometry and paint order

`src/frontend/splitdecoration.h` is pure paint geometry in drawable pixels.
It consumes existing DividerBox rectangles without changing them, pane cell
counts, scrollbar tracks, drag hitboxes or tab/split layout. The middle color is
exactly borderColor(stripPalette), the same theme color used for the menu/tab
strip and own window frame. The dark edge derives from half those RGB channels.

Each divider gets a one-physical-pixel dark edge facing its neighboring
terminals. Gutters thinner than three pixels remain dark rather than emitting
negative-width middles. All dark base rectangles are painted FIRST; then ALL
middle fills, perpendicular intersections and one-pixel T seam bridges are
painted. T junctions in all four directions and cross intersections therefore
have solid themed middles, not a dark stripe running through another divider.
Corners are deliberately straight; rounded corners are deferred to user review.

In the native loop gutters are drawn before pane rendering. Pane viewports do
not include divider interiors, so the theme survives the pane fill. Existing
external scrollbar tracks/thumbs and wrapped-line marks then draw in their
reserved gutter portions and remain visible. The former opaque full-gutter
paint after all panes is removed; there is no second paint hiding those marks.
The shared SDL renderer implementation and web frontend are unchanged.

### Original outer-ring policy (superseded by the follow-up above)

A one-pixel dark outer edge was painted over the terminal-area boundary only
where it is not already supplied by the app's own frame. Ordinary system-frame
windows get a thin terminal-area outline. Own-frame windows do not add another
left/right/bottom ring; a seam immediately below a visible tab strip is allowed.
Maximized/fullscreen windows have no new outline against a screen edge, only
the internal seam below a visible strip. Divider fills clip inside any such
outer ring. The one-pixel paint overlay does not reserve cells/change layout.

The existing own-frame inset and border helpers now explicitly suppress their
four-pixel border in fullscreen, just as for maximized. The SDL hit geometry
also treats fullscreen as maximized (no edge resize grip). Ordinary-window
geometry, divider hitboxes and dragging are not changed. The optional helper
parameters preserve existing callers' defaults.

## Verification

- Pure tests rasterize rectangle layers to check dark edge width, all four T
  directions, solid cross overlap, ordinary/own/maximized/fullscreen outer
  masks, one-/two-/three-pixel clipped/tiny cases and exact one-physical-pixel
  high-DPI-style edges. Current palette is read on each paint, no color cache.
- Actual SDL dummy/software pixel readback paints gutters then a real pane:
  themed middle and T seam survive, one-pixel edge remains, existing scrollbar
  thumb AND wrapped-line marker remain visible. Repaint blue -> green in the
  same renderer verifies live theme-dependent pixels.
- Existing window inset/hit regression tests plus explicit fullscreen tests
  pass. Existing full native/settings/web/shared renderer regressions pass.
- Warning-free full build; **151/151 tests passed**, 134.45 seconds, excluding
  only the two older prohibited-pkill layout scripts. This local branch still
  contains two unintegrated pure pump-policy tests unrelated to this patch;
  the integrator's test count may therefore differ.
- Changed C++ clang-formatted; local clang-tidy unavailable (run-clang-tidy
  missing). No physical desktop/high-DPI display visual check performed.
  User visual confirmation, especially corners/outer-line taste, is pending.
