#pragma once

#include "rgb8.h"

struct CursorState {
    enum class Shape { Block, Underline, Bar };

    int column = 0;
    int row = 0;
    Shape shape = Shape::Underline;
    bool visible = false;
    bool blink = true;
    // False while the window is inactive: every shape is drawn as a hollow
    // rectangle. Blinking is the caller's job and normally stops too.
    bool focused = true;
    Rgb8 color{255, 255, 255};
    // Gliding cursor: the cursor is drawn at (column + columnOffset, row +
    // rowOffset), in cells (negative: still left of or above its cell). Zero
    // (the default) is the exact cell, drawn as above; nonzero draws a
    // translucent block that moves, and the cell is not inverted.
    float columnOffset = 0;
    float rowOffset = 0;
};
