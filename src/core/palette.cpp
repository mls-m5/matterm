#include "palette.h"

#include <algorithm>
#include <utility>

Palette::Palette() {
    const std::array<Rgb8, 16> base{{{46, 52, 54},
                                     {204, 0, 0},
                                     {78, 154, 6},
                                     {196, 160, 0},
                                     {52, 101, 164},
                                     {117, 80, 123},
                                     {6, 152, 154},
                                     {211, 215, 207},
                                     {85, 87, 83},
                                     {239, 41, 41},
                                     {138, 226, 52},
                                     {252, 233, 79},
                                     {114, 159, 207},
                                     {173, 127, 168},
                                     {52, 226, 226},
                                     {238, 238, 236}}};
    std::copy(base.begin(), base.end(), colors.begin());
    constexpr std::array<std::uint8_t, 6> levels{0, 95, 135, 175, 215, 255};
    for (int i = 16; i < 232; ++i) {
        const int n = i - 16;
        colors[std::size_t(i)] = {levels[std::size_t(n / 36)],
                                  levels[std::size_t(n / 6 % 6)],
                                  levels[std::size_t(n % 6)]};
    }
    for (int i = 232; i < 256; ++i) {
        const auto level = std::uint8_t(8 + 10 * (i - 232));
        colors[std::size_t(i)] = {level, level, level};
    }
}
Rgb8 Palette::resolve(ColorReference color,
                      bool foregroundRole) const noexcept {
    switch (color.kind) {
    case ColorReference::Kind::Rgb:
        return color.value;
    case ColorReference::Kind::Indexed:
        return colors[color.value.r];
    default:
        return foregroundRole ? foreground : background;
    }
}
ResolvedColors Palette::resolve(const FChar &cell) const noexcept {
    auto fg = resolve(cell.foregroundColor(), true);
    auto bg = resolve(cell.backgroundColor(), false);
    if (hasAttribute(cell.attributes, CellAttribute::Inverse)) {
        std::swap(fg, bg);
    }
    if (hasAttribute(cell.attributes, CellAttribute::Conceal)) {
        fg = bg;
    }
    if (hasAttribute(cell.attributes, CellAttribute::Faint)) {
        fg = {std::uint8_t((int(fg.r) + bg.r) / 2),
              std::uint8_t((int(fg.g) + bg.g) / 2),
              std::uint8_t((int(fg.b) + bg.b) / 2)};
    }
    return {fg, bg};
}
