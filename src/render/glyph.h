#pragma once

#include <SDL.h>

// A rasterized glyph's location and layout metrics, measured in pixels.
// This SDL-specific record is backend data, not terminal cell content.
struct Glyph {
    SDL_Rect source{0, 0, 0, 0};
    int bearingX = 0; // Left edge relative to the pen position.
    int bearingY = 0; // Top edge above the baseline (positive upwards).
    int advanceX = 0;

    // An empty source rectangle is valid, for example for a space.
};
