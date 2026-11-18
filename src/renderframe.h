#pragma once

#include "cellgridview.h"
#include "cursorstate.h"

// All views are borrowed for the duration of RenderBackend::render only.
struct RenderFrame {
    CellGridView cells;
    CursorState cursor;
};
