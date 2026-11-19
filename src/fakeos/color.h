#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>

namespace fakeos {

inline Rgb mix(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto c = [&](int x, int y) { return std::uint8_t(x + (y - x) * t + 0.5); };
    return {c(a.r, b.r), c(a.g, b.g), c(a.b, b.b)};
}

// Hue in turns (any real), saturation and value in [0, 1].
inline Rgb hsv(double hue, double saturation, double value) {
    hue -= std::floor(hue);
    const double h = hue * 6;
    const int i = int(h);
    const double f = h - i;
    const double p = value * (1 - saturation);
    const double q = value * (1 - saturation * f);
    const double t = value * (1 - saturation * (1 - f));
    double r = value, g = t, b = p;
    switch (i % 6) {
    case 1:
        r = q;
        g = value;
        b = p;
        break;
    case 2:
        r = p;
        g = value;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = value;
        break;
    case 4:
        r = t;
        g = p;
        b = value;
        break;
    case 5:
        r = value;
        g = p;
        b = q;
        break;
    default:
        break;
    }
    return {std::uint8_t(r * 255 + 0.5),
            std::uint8_t(g * 255 + 0.5),
            std::uint8_t(b * 255 + 0.5)};
}

// A deterministic hash of a cell, in [0, 1).
inline double cellNoise(int x, int y, std::uint32_t salt = 0) {
    std::uint32_t h = std::uint32_t(x) * 374761393u +
                      std::uint32_t(y) * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return double(h & 0xFFFFFF) / double(0x1000000);
}

} // namespace fakeos
