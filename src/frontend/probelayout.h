#pragma once

// The picture --window-test shows to find out whether the window can be transparent: a
// half-transparent red margin round a solid box, and a fully transparent rim outside the red.
// Pure rectangles (window pixels), so the drawing code of each renderer variant shares them.
#include <algorithm>

namespace frontend {

struct ProbeRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

struct ProbeLayout {
    ProbeRect red; // Half-transparent red; the area between `red` and the window edge is transparent.
    ProbeRect box; // Solid, inside the red.
};

inline ProbeLayout probeLayout(int width, int height, int margin) {
    ProbeLayout layout;
    const int rim = margin / 2;
    if (margin <= 0 || width <= 2 * margin || height <= 2 * margin) {
        return layout; // Too small to show anything: both empty.
    }
    layout.red = {rim, rim, width - 2 * rim, height - 2 * rim};
    layout.box = {margin, margin, width - 2 * margin, height - 2 * margin};
    return layout;
}

} // namespace frontend
