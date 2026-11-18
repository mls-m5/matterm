#pragma once

#include "cellgridview.h"
#include "cursorstate.h"
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
    ScrollbarState scrollbar;
};
