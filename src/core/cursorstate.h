#pragma once

#include "rgb8.h"

struct CursorState {
    enum class Shape { Block, Underline, Bar };

    int column = 0;
    int row = 0;
    Shape shape = Shape::Underline;
    bool visible = false;
    bool blink = true;
    Rgb8 color{255, 255, 255};
};
