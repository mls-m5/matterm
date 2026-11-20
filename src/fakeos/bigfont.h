#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fakeos {

// A 5x5 block font for A-Z, 0-9, '!' and '.', used by animations that draw
// big words. Lower case maps to upper case; anything else is blank.
inline const char *const *bigGlyph(char c) {
    static const char *const blank[5] = {
        "     ", "     ", "     ", "     ", "     "};
    static const char *const glyphs[][5] = {
        {" ### ", "#   #", "#####", "#   #", "#   #"}, // A
        {"#### ", "#   #", "#### ", "#   #", "#### "}, // B
        {" ####", "#    ", "#    ", "#    ", " ####"}, // C
        {"#### ", "#   #", "#   #", "#   #", "#### "}, // D
        {"#####", "#    ", "#### ", "#    ", "#####"}, // E
        {"#####", "#    ", "#### ", "#    ", "#    "}, // F
        {" ####", "#    ", "#  ##", "#   #", " ####"}, // G
        {"#   #", "#   #", "#####", "#   #", "#   #"}, // H
        {"#####", "  #  ", "  #  ", "  #  ", "#####"}, // I
        {"  ###", "   # ", "   # ", "#  # ", " ##  "}, // J
        {"#   #", "#  # ", "###  ", "#  # ", "#   #"}, // K
        {"#    ", "#    ", "#    ", "#    ", "#####"}, // L
        {"#   #", "## ##", "# # #", "#   #", "#   #"}, // M
        {"#   #", "##  #", "# # #", "#  ##", "#   #"}, // N
        {" ### ", "#   #", "#   #", "#   #", " ### "}, // O
        {"#### ", "#   #", "#### ", "#    ", "#    "}, // P
        {" ### ", "#   #", "# # #", "#  # ", " ## #"}, // Q
        {"#### ", "#   #", "#### ", "#  # ", "#   #"}, // R
        {" ####", "#    ", " ### ", "    #", "#### "}, // S
        {"#####", "  #  ", "  #  ", "  #  ", "  #  "}, // T
        {"#   #", "#   #", "#   #", "#   #", " ### "}, // U
        {"#   #", "#   #", "#   #", " # # ", "  #  "}, // V
        {"#   #", "#   #", "# # #", "## ##", "#   #"}, // W
        {"#   #", " # # ", "  #  ", " # # ", "#   #"}, // X
        {"#   #", " # # ", "  #  ", "  #  ", "  #  "}, // Y
        {"#####", "   # ", "  #  ", " #   ", "#####"}, // Z
        {" ### ", "#  ##", "# # #", "##  #", " ### "}, // 0
        {"  #  ", " ##  ", "  #  ", "  #  ", " ### "}, // 1
        {" ### ", "#   #", "  ## ", " #   ", "#####"}, // 2
        {"#### ", "    #", " ### ", "    #", "#### "}, // 3
        {"#  # ", "#  # ", "#####", "   # ", "   # "}, // 4
        {"#####", "#    ", "#### ", "    #", "#### "}, // 5
        {" ### ", "#    ", "#### ", "#   #", " ### "}, // 6
        {"#####", "   # ", "  #  ", " #   ", " #   "}, // 7
        {" ### ", "#   #", " ### ", "#   #", " ### "}, // 8
        {" ### ", "#   #", " ####", "    #", " ### "}, // 9
        {"  #  ", "  #  ", "  #  ", "     ", "  #  "}, // !
        {"     ", "     ", "     ", "     ", "  #  "}, // .
    };
    if (c >= 'a' && c <= 'z') {
        c = char(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return glyphs[c - 'A'];
    }
    if (c >= '0' && c <= '9') {
        return glyphs[26 + (c - '0')];
    }
    if (c == '!') {
        return glyphs[36];
    }
    if (c == '.') {
        return glyphs[37];
    }
    return blank;
}

// The lit cells of `text` in big letters: (x, y) with x growing 6 per letter.
// Width is 6 * letters - 1, height 5.
inline std::vector<std::pair<int, int>> bigText(const std::string &text) {
    std::vector<std::pair<int, int>> cells;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto *glyph = bigGlyph(text[i]);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) {
                if (glyph[y][x] == '#') {
                    cells.emplace_back(int(i) * 6 + x, y);
                }
            }
        }
    }
    return cells;
}

} // namespace fakeos
