#include "tty/color.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace tty {

int nearestXterm256(Rgb c) {
    // The 6x6x6 cube (16..231) and the 24 grays (232..255); pick the closer.
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    auto nearest = [&](int v) {
        int best = 0;
        for (int i = 1; i < 6; ++i) {
            if (std::abs(levels[i] - v) < std::abs(levels[best] - v)) {
                best = i;
            }
        }
        return best;
    };
    const int ri = nearest(c.r), gi = nearest(c.g), bi = nearest(c.b);
    const int cube = 16 + 36 * ri + 6 * gi + bi;
    auto dist = [&](int r, int g, int b) {
        return (c.r - r) * (c.r - r) + (c.g - g) * (c.g - g) +
               (c.b - b) * (c.b - b);
    };
    const int cubeDist = dist(levels[ri], levels[gi], levels[bi]);
    const int average = (c.r + c.g + c.b) / 3;
    const int grayIndex = std::clamp((average - 8 + 5) / 10, 0, 23);
    const int gray = 8 + 10 * grayIndex;
    return dist(gray, gray, gray) < cubeDist ? 232 + grayIndex : cube;
}

int nearestAnsi16(Rgb c) {
    static const std::array<Rgb, 16> palette = {{
        {0, 0, 0},
        {205, 0, 0},
        {0, 205, 0},
        {205, 205, 0},
        {0, 0, 238},
        {205, 0, 205},
        {0, 205, 205},
        {229, 229, 229},
        {127, 127, 127},
        {255, 0, 0},
        {0, 255, 0},
        {255, 255, 0},
        {92, 92, 255},
        {255, 0, 255},
        {0, 255, 255},
        {255, 255, 255},
    }};
    int best = 0, bestDistance = 1 << 30;
    for (int i = 0; i < 16; ++i) {
        const auto &p = palette[std::size_t(i)];
        const int d = (c.r - p.r) * (c.r - p.r) + (c.g - p.g) * (c.g - p.g) +
                      (c.b - p.b) * (c.b - p.b);
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void appendSgrColor(std::string &out, bool foreground, Rgb color, Caps caps) {
    if (caps.truecolor) {
        out += foreground ? "38;2;" : "48;2;";
        out += std::to_string(color.r) + ";" + std::to_string(color.g) + ";" +
               std::to_string(color.b);
    }
    else if (caps.colors256) {
        out += foreground ? "38;5;" : "48;5;";
        out += std::to_string(nearestXterm256(color));
    }
    else {
        const int index = nearestAnsi16(color);
        const int base =
            index < 8 ? (foreground ? 30 : 40) : (foreground ? 90 : 100);
        out += std::to_string(base + (index & 7));
    }
}

void appendSgrColor(std::string &out,
                    bool foreground,
                    Rgb color,
                    bool isDefault,
                    int index,
                    Caps caps) {
    if (isDefault) {
        out += foreground ? "39" : "49";
    }
    else if (index >= 0 && index < 16) {
        const int base =
            index < 8 ? (foreground ? 30 : 40) : (foreground ? 90 : 100);
        out += std::to_string(base + (index & 7));
    }
    else if (index >= 16 && index < 256 && caps.colors256) {
        out += foreground ? "38;5;" : "48;5;";
        out += std::to_string(index);
    }
    else {
        appendSgrColor(out, foreground, color, caps);
    }
}

} // namespace tty
