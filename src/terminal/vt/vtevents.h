#pragma once

#include "rgb8.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

class Palette;

// Views are valid only during the callback. Copy them before retaining them.
// Callbacks must not re-enter the parser. No OS actions are performed by it.
struct VtEvents {
    std::function<void(std::string_view)> reply;
    std::function<void(std::string_view)> title;
    // Optional active renderer palette; must outlive the parser/session.
    const Palette *palette = nullptr;
    // OSC 4 / OSC 104 requests. The client owns the palette and decides
    // whether to honor them; reset without an index restores every entry.
    std::function<void(std::size_t, Rgb8)> setPaletteColor;
    std::function<void(std::optional<std::size_t>)> resetPaletteColor;
};
