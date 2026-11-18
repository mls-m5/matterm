#include "democontent.h"

#include "cellbuffer.h"
#include "terminalmemory.h"

#include <algorithm>
#include <array>
#include <ostream>
#include <string>
#include <string_view>

namespace {
constexpr std::array<std::string_view, 5> banner{
    R"(                 _   _                      )",
    R"( _ __ ___   __ _| |_| |_ ___ _ __ _ __ ___  )",
    R"(| '_ ` _ \ / _` | __| __/ _ \ '__| '_ ` _ \ )",
    R"(| | | | | | (_| | |_| ||  __/ |  | | | | | |)",
    R"(|_| |_| |_|\__,_|\__|\__\___|_|  |_| |_| |_|)"};
constexpr Rgb8 background{14, 17, 22};
constexpr Rgb8 foreground{220, 225, 245};
constexpr Rgb8 accent{120, 220, 170};

std::u32string ascii(std::string_view text) {
    return std::u32string(text.begin(), text.end());
}
} // namespace

void demo::printBanner(std::ostream &output) {
    for (const auto line : banner) {
        output << line << '\n';
    }
    output << "matterm - a terminal in the making\n\n";
}

void demo::welcome(TerminalMemory &memory, std::size_t visibleRows) {
    const auto height =
        std::min(memory.capacity(), std::max<std::size_t>(20, visibleRows));
    CellBuffer canvas(memory.columns(), height);
    canvas.clear(FChar{U' ', foreground, background});
    canvas.draw(
        1, 0, U"matterm - a terminal in the making", accent, background);
    for (std::size_t y = 0; y < banner.size(); ++y) {
        canvas.draw(
            1, int(y) + 1, ascii(banner[y]), {160, 180, 250}, background);
    }
    canvas.draw(1,
                7,
                U"Agjpqy  ÅÄÖ åäö  0123456789  []{} <> /\\",
                foreground,
                background);
    canvas.draw(1,
                9,
                U"Per-cell backgrounds and foreground colors",
                accent,
                background);
    const auto gradientWidth = std::min<std::size_t>(
        48, memory.columns() > 2 ? memory.columns() - 2 : 0);
    for (std::size_t x = 0; x < gradientWidth; ++x) {
        const int t =
            gradientWidth > 1 ? int(x * 255 / (gradientWidth - 1)) : 0;
        const Rgb8 color{std::uint8_t(40 + t * 180 / 255),
                         std::uint8_t(60 + t * 100 / 255),
                         std::uint8_t(210 - t * 130 / 255)};
        canvas.draw(int(x) + 1, 10, U" ", foreground, color);
        canvas.draw(int(x) + 1, 11, U"#", color, background);
    }
    canvas.draw(
        1, 13, U"S / Space: toggle numbered output", foreground, background);
    canvas.draw(1,
                14,
                U"B: burst 300 rows   R: restore this welcome screen",
                foreground,
                background);
    canvas.draw(1,
                16,
                U"Wheel / PgUp / PgDn: scroll   Home / End: top / live",
                accent,
                background);
    canvas.draw(1, 17, U"Tab: cursor shape   Escape: exit", accent, background);
    canvas.draw(1,
                19,
                U"Demo only - shell and alternate screen come next.",
                {180, 185, 200},
                background);
    memory.clear();
    const auto view = canvas.view();
    for (std::size_t y = 0; y < height; ++y) {
        memory.append(view.row(y));
    }
}

void demo::appendNumberedRow(TerminalMemory &memory, std::uint64_t number) {
    const auto label = std::to_string(number);
    std::u32string text = U"Row ";
    text.append(label.begin(), label.end());
    text += U" | matterm block-ring scrollback | Agjpqy ÅÄÖ åäö";
    const auto color = number % 2 ? accent : foreground;
    memory.append(text, color, background);
}
