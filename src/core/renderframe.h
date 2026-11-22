#pragma once

#include "cellgridview.h"
#include "cursorstate.h"
#include "palette.h"
#include "scrollbarstate.h"

#include <cstddef>
#include <utility>
#include <vector>

// All views are borrowed for the duration of RenderBackend::render only.
struct RenderFrame {
    CellGridView cells;
    CursorState cursor;
    // Offset of the first supplied row. Supply extra rows for partial
    // scrolling.
    float verticalOffsetPixels = 0;
    // -1 uses the drawable height; otherwise clip the text to this pixel
    // height.
    int textHeightPixels = -1;
    ScrollbarState scrollbar{};
    // Presentation clock is caller-owned, just like cursor blinking.
    bool textBlinkVisible = true;
    // Borrowed during render only. Null selects the backend's dark default.
    const Palette *palette = nullptr;
    // Half-open row-major interval in cells, relative to the supplied view.
    std::size_t selectionBegin = 0;
    std::size_t selectionEnd = 0;
    // Presentation-only underline overlay, with the same interval convention.
    // Does not modify cell attributes (for example while previewing a link).
    std::size_t underlineBegin = 0;
    std::size_t underlineEnd = 0;
    // More half-open row-major intervals: extra selected cells (a rectangular selection is one
    // interval per row) drawn like the selection, and search matches drawn in a highlight color.
    std::vector<std::pair<std::size_t, std::size_t>> selectionRanges;
    std::vector<std::pair<std::size_t, std::size_t>> matchRanges;
    // One entry per supplied row: the row continues on the next one (soft wrap). Empty: unknown.
    // Prepared for the wrapped-line marker (docs/SCROLLING_AND_REFLOW.md); no backend draws it yet.
    std::vector<bool> wrapMarks;
};
