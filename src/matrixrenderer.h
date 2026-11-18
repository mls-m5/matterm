#pragma once

#include "cellgridview.h"

class MatrixRenderer {
public:
    MatrixRenderer() = default;
    MatrixRenderer(const MatrixRenderer &) = delete;
    MatrixRenderer &operator=(const MatrixRenderer &) = delete;

    // Planned OpenGL entry point. Draws synchronously from the borrowed grid
    // into the current context's framebuffer. SDL handles the buffer swap.
    void drawFrame(CellGridView grid, int drawableWidth, int drawableHeight);
};
