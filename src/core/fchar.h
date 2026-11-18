#pragma once

#include "rgb8.h"

// First-pass cell format: one Unicode code point in one column.
// The terminal model will need explicit support for wide cells and clusters.
struct FChar {
    char32_t codePoint = U' ';
    Rgb8 foreground{255, 255, 255};
    Rgb8 background{0, 0, 0};
};
