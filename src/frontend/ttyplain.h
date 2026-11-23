#pragma once

// A plain painter of a ScreenImage to a host terminal: only the rows that
// changed are written, with the cursor moved and the colours set as truecolor
// or 256 colours. It is the stand-in until the shared src/tty layer
// (docs/tasks/attach-tty.md part 1 and 2) draws matterm's cell buffer; the mode
// only knows ScreenImage, so the swap is local. Pure: it returns the bytes, the
// caller writes them.
#include "ttyscreen.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace frontend::tty {

struct PlainCaps {
    bool truecolor = true;
};

inline void appendUtf8(std::string &out, char32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        cp = U' ';
    }
    if (cp < 0x80) {
        out += char(cp);
    }
    else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

// The xterm 256-colour palette entry nearest to an RGB colour (the 6x6x6 cube
// or the grey ramp).
inline int nearest256(Rgb8 c) {
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    const auto cubeIndex = [](int v) {
        int best = 0;
        for (int i = 1; i < 6; ++i) {
            if (std::abs(v - levels[i]) < std::abs(v - levels[best])) {
                best = i;
            }
        }
        return best;
    };
    const int r = cubeIndex(c.r), g = cubeIndex(c.g), b = cubeIndex(c.b);
    const int cube = 16 + 36 * r + 6 * g + b;
    const auto distance = [&](int rr, int gg, int bb) {
        return (rr - c.r) * (rr - c.r) + (gg - c.g) * (gg - c.g) +
               (bb - c.b) * (bb - c.b);
    };
    const int cubeDistance = distance(levels[r], levels[g], levels[b]);
    const int average = (c.r + c.g + c.b) / 3;
    int grey = (average - 8 + 5) / 10; // 0..23: 8, 18, ... 238.
    grey = grey < 0 ? 0 : grey > 23 ? 23 : grey;
    const int greyValue = 8 + 10 * grey;
    if (distance(greyValue, greyValue, greyValue) < cubeDistance) {
        return 232 + grey;
    }
    return cube;
}

class PlainPainter {
public:
    explicit PlainPainter(PlainCaps caps = {})
        : caps_(caps) {}
    void setCaps(PlainCaps caps) {
        caps_ = caps;
        invalidate();
    }
    // The terminal's content is not what was painted (it was resized, or
    // something else drew on it).
    void invalidate() {
        previous_ = ScreenImage();
        cursorKnown_ = false;
    }

    // The bytes that turn what the terminal shows into `image`; empty when
    // nothing changed.
    std::string paint(const ScreenImage &image) {
        std::string out;
        const bool same =
            previous_.columns == image.columns && previous_.rows == image.rows;
        if (!same) {
            out += "\x1b[0m\x1b[2J";
        }
        bool any = !same;
        std::string body;
        Style style;
        bool styleKnown = false;
        for (int y = 0; y < image.rows; ++y) {
            if (same && rowEqual(previous_, image, y)) {
                continue;
            }
            any = true;
            body += "\x1b[" + std::to_string(y + 1) + ";1H";
            for (int x = 0; x < image.columns; ++x) {
                const FChar &cell = image.at(x, y);
                const Style wanted{
                    cell.foreground, cell.background, cell.attributes};
                if (!styleKnown || !(wanted == style)) {
                    body += sgr(wanted);
                    style = wanted;
                    styleKnown = true;
                }
                appendUtf8(body, cell.codePoint < 32 ? U' ' : cell.codePoint);
            }
        }
        const bool cursorChanged =
            !cursorKnown_ || cursorVisible_ != image.cursorVisible ||
            (image.cursorVisible && (cursorColumn_ != image.cursorColumn ||
                                     cursorRow_ != image.cursorRow));
        if (!any && !cursorChanged) {
            return {};
        }
        out +=
            "\x1b[?25l"; // No flicker of the cursor while the rows are written.
        out += body;
        if (styleKnown) {
            out += "\x1b[0m";
        }
        if (image.cursorVisible) {
            out += "\x1b[" + std::to_string(image.cursorRow + 1) + ";" +
                   std::to_string(image.cursorColumn + 1) + "H\x1b[?25h";
        }
        previous_ = image;
        cursorKnown_ = true;
        cursorVisible_ = image.cursorVisible;
        cursorColumn_ = image.cursorColumn;
        cursorRow_ = image.cursorRow;
        return out;
    }

private:
    struct Style {
        Rgb8 fg{}, bg{};
        CellAttribute attributes = CellAttribute::None;
        bool operator==(const Style &o) const {
            return fg.r == o.fg.r && fg.g == o.fg.g && fg.b == o.fg.b &&
                   bg.r == o.bg.r && bg.g == o.bg.g && bg.b == o.bg.b &&
                   attributes == o.attributes;
        }
    };
    static bool rowEqual(const ScreenImage &a, const ScreenImage &b, int y) {
        for (int x = 0; x < a.columns; ++x) {
            const FChar &p = a.at(x, y), &q = b.at(x, y);
            if (p.codePoint != q.codePoint || p.attributes != q.attributes ||
                p.foreground.r != q.foreground.r ||
                p.foreground.g != q.foreground.g ||
                p.foreground.b != q.foreground.b ||
                p.background.r != q.background.r ||
                p.background.g != q.background.g ||
                p.background.b != q.background.b) {
                return false;
            }
        }
        return true;
    }
    std::string color(int base, Rgb8 c) const {
        if (caps_.truecolor) {
            return ";" + std::to_string(base) + ";2;" + std::to_string(c.r) +
                   ";" + std::to_string(c.g) + ";" + std::to_string(c.b);
        }
        return ";" + std::to_string(base) + ";5;" +
               std::to_string(nearest256(c));
    }
    std::string sgr(const Style &s) const {
        std::string out = "\x1b[0";
        if (hasAttribute(s.attributes, CellAttribute::Bold)) {
            out += ";1";
        }
        if (hasAttribute(s.attributes, CellAttribute::Italic)) {
            out += ";3";
        }
        if (hasAttribute(s.attributes, CellAttribute::Underline)) {
            out += ";4";
        }
        if (hasAttribute(s.attributes, CellAttribute::Strike)) {
            out += ";9";
        }
        out += color(38, s.fg);
        out += color(48, s.bg);
        out += "m";
        return out;
    }

    PlainCaps caps_;
    ScreenImage previous_;
    bool cursorKnown_ = false;
    bool cursorVisible_ = false;
    int cursorColumn_ = 0;
    int cursorRow_ = 0;
};

} // namespace frontend::tty
