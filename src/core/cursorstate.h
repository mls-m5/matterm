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
};
