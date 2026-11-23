#pragma once

#include "fchar.h"

#include <array>
#include <optional>

struct ResolvedColors {
    Rgb8 foreground;
    Rgb8 background;
};

// Caller-owned configuration. Changing entries affects existing indexed/default
// cells on the next render, not explicit RGB cells. No GUI or file
// dependencies.
class Palette {
public:
    Palette(); // Dark default with a GNOME/Tango-inspired 16-color palette.
    std::array<Rgb8, 256> colors;
    Rgb8 foreground{238, 238, 236};
    Rgb8 background{30, 30, 30};
    // For the window's own frame (tab strip, border) only, never for a
    // terminal: the colour of the strip, which a theme sets
    // (src/frontend/themes.h). Unset: derived from foreground and background as
    // always.
    std::optional<Rgb8> frame;

    Rgb8 resolve(ColorReference color, bool foregroundRole) const noexcept;
    ResolvedColors resolve(const FChar &cell) const noexcept;
};
