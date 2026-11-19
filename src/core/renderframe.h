#pragma once

#include "cellgridview.h"
#include "cursorstate.h"
#include "palette.h"
#include "scrollbarstate.h"

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
};
