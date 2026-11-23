#pragma once

// The one place the tty layer meets matterm's own types: matterm's cell buffer
// (FChar rows, colours that are RGB, palette-indexed or the default) and
// cursor drawn through a tty::Screen. The rest of src/tty knows none of them.
#include "cellgridview.h"
#include "cursorstate.h"
#include "palette.h"
#include "tty/screen.h"

namespace tty {

// Writes the cells of `grid` into the screen's canvas with their top-left at
// (originX, originY), clipped to the screen. Indexed and default colours are
// handed on as such (the host terminal's theme picks them); RGB colours as
// RGB. A NUL cell is a blank. `palette` gives the RGB fallback of indexed and
// default colours for terminals without 256 colours.
void drawCells(Screen &screen,
               const CellGridView &grid,
               const Palette &palette,
               int originX = 0,
               int originY = 0);

// The cursor of the grid, at its cell (no gliding, no colour).
Cursor toCursor(const CursorState &state, int originX = 0, int originY = 0);

} // namespace tty
