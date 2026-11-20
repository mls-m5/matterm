#include "canvas.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace fakeos {

void appendUtf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    }
    else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::u32string decodeUtf8(const std::string &text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = (unsigned char)text[i];
        std::size_t n = b < 0x80    ? 1
                        : b >= 0xF0 ? 4
                        : b >= 0xE0 ? 3
                        : b >= 0xC0 ? 2
                                    : 1;
        char32_t cp = n == 1 ? b : b & (0xFF >> (n + 1));
        for (std::size_t k = 1; k < n && i + k < text.size(); ++k) {
            cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

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

AsciiCanvas::AsciiCanvas(int columns, int rows, Capabilities caps)
    : columns_(std::max(1, columns))
    , rows_(std::max(1, rows))
    , caps_(caps)
    , cells_(std::size_t(columns_) * std::size_t(rows_))
    , shown_(cells_.size()) {}

void AsciiCanvas::resize(int columns, int rows) {
    columns_ = std::max(1, columns);
    rows_ = std::max(1, rows);
    cells_.assign(std::size_t(columns_) * std::size_t(rows_), Cell{});
    shown_ = cells_;
    known_ = false;
}

void AsciiCanvas::setCapabilities(Capabilities caps) {
    caps_ = caps;
    known_ = false;
}

void AsciiCanvas::clear(Rgb background) {
    Cell blank;
    blank.background = background;
    std::fill(cells_.begin(), cells_.end(), blank);
}

void AsciiCanvas::put(int x, int y, Cell cell) {
    if (x >= 0 && x < columns_ && y >= 0 && y < rows_) {
        cells_[std::size_t(y) * std::size_t(columns_) + std::size_t(x)] = cell;
    }
}

void AsciiCanvas::put(int x, int y, char32_t cp, Rgb fg, Rgb bg, bool bold) {
    put(x, y, Cell{cp, fg, bg, bold});
}

void AsciiCanvas::text(
    int x, int y, const std::string &utf8, Rgb fg, Rgb bg, bool bold) {
    for (const auto cp : decodeUtf8(utf8)) {
        put(x++, y, cp, fg, bg, bold);
    }
}

const Cell &AsciiCanvas::at(int x, int y) const {
    return cells_.at(std::size_t(y) * std::size_t(columns_) + std::size_t(x));
}

std::string AsciiCanvas::wordAt(int x, int y) const {
    if (x < 0 || x >= columns_ || y < 0 || y >= rows_) {
        return {};
    }
    auto isWord = [&](int column) {
        const auto cp = at(column, y).codePoint;
        return cp > U' ' && cp != U'(' && cp != U')' && cp != U'[' &&
               cp != U']' && cp != U'<' && cp != U'>' && cp != U'|' &&
               cp != U',';
    };
    if (!isWord(x)) {
        return {};
    }
    int begin = x, end = x;
    while (begin > 0 && isWord(begin - 1)) {
        --begin;
    }
    while (end + 1 < columns_ && isWord(end + 1)) {
        ++end;
    }
    std::string word;
    for (int column = begin; column <= end; ++column) {
        appendUtf8(word, at(column, y).codePoint);
    }
    return word;
}

void AsciiCanvas::appendColor(std::string &out,
                              bool foreground,
                              Rgb color) const {
    if (caps_.truecolor) {
        out += foreground ? "38;2;" : "48;2;";
        out += std::to_string(color.r) + ";" + std::to_string(color.g) + ";" +
               std::to_string(color.b);
    }
    else if (caps_.colors256) {
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

std::string AsciiCanvas::flush(bool forceFull) {
    std::string out;
    const bool full = forceFull || !known_;
    int cursorX = -1, cursorY = -1;
    bool haveStyle = false;
    Cell style;
    for (int y = 0; y < rows_; ++y) {
        for (int x = 0; x < columns_; ++x) {
            const auto index =
                std::size_t(y) * std::size_t(columns_) + std::size_t(x);
            const Cell &cell = cells_[index];
            if (!full && cell == shown_[index]) {
                continue;
            }
            if (cursorX != x || cursorY != y) {
                out += "\x1b[" + std::to_string(y + 1) + ";" +
                       std::to_string(x + 1) + "H";
            }
            if (!haveStyle || cell.foreground != style.foreground ||
                cell.background != style.background ||
                cell.bold != style.bold) {
                out += cell.bold ? "\x1b[0;1;" : "\x1b[0;";
                appendColor(out, true, cell.foreground);
                out += ";";
                appendColor(out, false, cell.background);
                out += "m";
                style = cell;
                haveStyle = true;
            }
            appendUtf8(out, cell.codePoint < 0x20 ? U' ' : cell.codePoint);
            cursorX = x + 1;
            cursorY = y;
            if (cursorX >= columns_) { // Past the last column: position again.
                cursorX = -1;
            }
        }
    }
    if (haveStyle) {
        out += "\x1b[0m";
    }
    shown_ = cells_;
    known_ = true;
    return out;
}

} // namespace fakeos
