# Local text selection

The native `matterm` PTY frontend and browser client use the same CPU-only
selection policy (`src/frontend/textselection.h`). The standalone demos are
unchanged.

## Controls

- Left click/drag selects cells, with inclusive endpoints in either direction.
- Shift+click moves the selection end while keeping its current anchor.
  Shift+drag continues cell-by-cell from that anchor, even if Shift is released
  during the drag. Without a valid selection, Shift+click starts a new one.
- Ctrl+Shift+A selects all currently retained history plus the active screen,
  regardless of the scroll viewport. This is a snapshot range, not a request to
  follow future output. In the alternate screen, only that screen is selected.
- Double click selects a word. Dragging extends to whole words in either
  direction, preserving the complete word where the drag started.
- Triple click selects a logical line, including its soft-wrapped rows.
  Dragging extends by complete logical lines in either direction.
- Shift+click after a word/line selection preserves its current anchor but
  returns to cell-granular extension. A plain click starts a new selection.
- Ctrl+Shift+C copies UTF-8. Soft-wrapped rows are joined without a newline;
  explicit line breaks are retained. Existing trailing-space copy policy is
  unchanged. Copy is bounded to **1 MiB**; oversized selections are not
  truncated, and both clients report `selection exceeds 1 MiB` in the title.

When a program requests application mouse reports, unmodified mouse events
still go to the program. Hold Shift to initiate local selection instead. Once
a local drag begins it stays local until release, even if Shift is released.
Navigation and selection do not send terminal input or change stored text.

## Word and line boundaries

Words contain ASCII letters, digits and the path/URL characters
`_ - . / : @ ~ % + = ? & #`. This intentionally treats a path or URL as a single
word; it does **not** make URLs clickable or execute anything.

For deterministic, locale-independent international text handling, non-ASCII
cells are conservatively word-like except Unicode whitespace and punctuation
ranges U+200B–206F, U+2E00–2E7F, U+3001–303F and the fullwidth punctuation
subranges encoded in the helper. This is a heuristic, not full Unicode word
breaking; for example adjacent emoji are word-like. Clicking a separator
selects its contiguous whitespace or punctuation run. Boundaries may cross
soft wraps but never an explicit newline. Logical lines stop at the oldest
retained row if their beginning was evicted. There is still one scalar per
cell, with no combining-cluster or wide-cell selection support.

Resize, reset, screen switching, regional row movement or eviction of selected
content invalidate the selection through `selectionEpoch` or retained-row
bounds. A stale selection is never reused as a Shift anchor and cannot be
copied or highlighted.

## Implementation / agent handoff

Branch: `agent3-selection`; base:
`6bb43ccb86d64f662cbc4d7b65e7ae9cc5e51139`.

- `Selection` moved from SDL input helpers to `src/frontend/textselection.h`.
  Shared logic covers clicks, extension, bounds, select-all, validation, bounded
  text extraction and frame highlight clipping. `copySelection` remains the
  small SDL clipboard adapter in `sdlinput.h`.
- Two read-only model helpers were necessary to inspect offscreen retained text
  and wrap flags without moving the viewport: `textCodePoint(TextCell)` returns
  an optional scalar and `textRowWrapped(row)` returns a wrap flag. They use the
  same absolute row IDs as `copyText`; invalid IDs return nullopt/false. No model
  resize, storage, copy policy or parser behavior was changed.
- Shared-file edits in `app/ptyfrontend.cpp` and `web/main.cpp` are restricted to
  selection, keyboard shortcuts and copy failure reporting. Review those hunks
  when integrating alongside browser changes.
- `test/textselection_test.cpp` adds 11 CPU-only unit cases, registered in
  `test/CMakeLists.txt`, covering both directions, Shift anchors, unit drag,
  Unicode/path words, wraps versus hard lines, history/eviction, epoch changes,
  viewport highlight clipping, invalid endpoints and >1 MiB copy rejection.

Verification (system DejaVu font, bundled download disabled):

```sh
cmake -S . -B build -DMATTERM_BUNDLE_FONT=OFF \
  -DTEST_FONT_FILE=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
cmake --build build -j4
ctest --test-dir build --output-on-failure

cmake -S . -B build/selection-cpu -DMATTERM_BUILD_APP=OFF \
  -DMATTERM_BUILD_DEMO=OFF -DMATTERM_BUILD_RENDERER=OFF
cmake --build build/selection-cpu -j4
ctest --test-dir build/selection-cpu --output-on-failure
```

Results: **33/33** native/default-feature tests and **20/20** graphics-disabled
CPU tests passed. `emcmake` is unavailable in this sandbox: browser compilation,
headless-browser gesture tests and real-display interaction are not verified.
The existing unrelated `lib/fast-json` working-tree gitlink mismatch was left
untouched and is not part of this commit. No push was performed.
