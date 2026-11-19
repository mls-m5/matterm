#pragma once

#include <algorithm>
#include <cmath>

namespace frontend {

struct WindowSize {
    int width = 0;
    int height = 0;
    bool operator==(const WindowSize &other) const {
        return width == other.width && height == other.height;
    }
};

// Window size (in window units) that holds a whole number of cells, so no
// partial cell is left at the right or bottom edge. `strip` is the width
// reserved for the scrollbar. The drawable may be larger than the window on
// HiDPI screens; the ratio is taken from the two sizes. Shrinks to whole cells,
// keeping at least one cell. A non-integer ratio can leave a
// fraction of a pixel, which is unavoidable.
inline WindowSize snappedWindowSize(WindowSize window,
                                    WindowSize drawable,
                                    int cellWidth,
                                    int cellHeight,
                                    int strip) {
    if (window.width <= 0 || window.height <= 0 || drawable.width <= 0 ||
        drawable.height <= 0 || cellWidth <= 0 || cellHeight <= 0) {
        return window;
    }
    const double ratioX = double(drawable.width) / window.width;
    const double ratioY = double(drawable.height) / window.height;
    const int columns =
        std::max(1, (drawable.width - strip) / cellWidth);
    const int rows = std::max(1, drawable.height / cellHeight);
    return {int(std::lround((columns * cellWidth + strip) / ratioX)),
            int(std::lround(rows * cellHeight / ratioY))};
}

} // namespace frontend
