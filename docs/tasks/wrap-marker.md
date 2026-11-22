# Wrapped-line marker (agent2)

Goal: a row that continues on the next one (a soft wrap: a long line, or a line the reflow laid out
again) gets a small mark in the pane's right gutter, so it can be told from a row that ends there.
The API exists since the reflow work: `RenderFrame::wrapMarks`, `TerminalModel::viewWrapMarks()`
(`docs/SCROLLING_AND_REFLOW.md`, "Wrapped-line marker"). Nothing draws it yet.

Scope
- Fill `RenderFrame::wrapMarks` where a frame is drawn (native `app/ptyfrontend.cpp`, browser
  `web/main.cpp`); the model's own `frame()` stays as it is (other callers only read the scrollbar).
- `SdlBackend` draws one small filled triangle per marked row in the gutter: no cell is covered, no
  column is added, the grid and the layout stay as they are. The browser client uses the same backend.
- No room for it (a pane without a reserved strip): not drawn.
- Frame signatures include the marks, so a change redraws and an idle window still sleeps.
- Pure geometry and style in one small header (`src/render/wrapmark.h`) with unit tests; the style
  (size, colour, side, shape) is a handful of constants there and nowhere else.
- Measure with `docs/CPU.md` (idle loop, a Claude-Code-like load) before and after.

Out of scope: a setting or a flag (none without asking the owner), a glyph variant, marks on the
alternate screen (rows there never wrap-flag), changes to reflow, selection or copy.

Owned paths: `src/render/wrapmark.h` (new), `src/render/sdlbackend.cpp`, `src/frontend/framesignature.h`,
`test/wrapmark_test.cpp`, `test/sdlbackend_test.cpp`, `docs/SCROLLING_AND_REFLOW.md`.
Shared (report edits, keep them minimal): `app/ptyfrontend.cpp`, `web/main.cpp` (one line each).

Acceptance: ctest green, new tests fail before and pass after, warning-free, clang-format and
clang-tidy on the changed files. The GUI and the browser cannot be run here: say so; the pixel test
with the software renderer stands in. The owner's open questions (appearance) are listed in
`docs/handover/features/agent2-wrap-marker.md` with the default chosen.
