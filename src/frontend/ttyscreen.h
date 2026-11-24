#pragma once

// Small helpers for drawing the mode's own screens (the list, the hint line)
// into the shared terminal layer's canvas (src/tty). The pane itself is drawn
// by tty::drawCells.
#include "tty/canvas.h"
#include "tty/utf8.h"
#include "tty/width.h"

#include <algorithm>
#include <string>

namespace frontend::ttymode {

inline constexpr ::tty::Rgb kScreenFg{220, 220, 220};
inline constexpr ::tty::Rgb kScreenBg{0, 0, 0};

inline bool unsafeScreenCodePoint(char32_t cp) {
    return cp < 32 || (cp >= 127 && cp <= 159) ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) ||
           cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff);
}

// Writes `text` from (x, y), clipped to the row, with control characters shown
// as blanks. Returns the column after the last character.
inline int put(::tty::Canvas &canvas,
               int x,
               int y,
               const std::string &text,
               ::tty::Rgb fg = kScreenFg,
               ::tty::Rgb bg = kScreenBg,
               bool bold = false,
               int maxWidth = -1) {
    const int end =
        maxWidth < 0
            ? canvas.columns()
            : x + std::min(maxWidth, std::max(0, canvas.columns() - x));
    // File names and metadata are untrusted. Bound decoding before allocating;
    // reuse tty's decoder/width tables rather than parsing terminal escapes.
    for (char32_t cp : ::tty::decodeUtf8(text.substr(0, 4096))) {
        if (unsafeScreenCodePoint(cp)) {
            cp = U' ';
        }
        const int width = ::tty::cellWidth(cp);
        if (width == 0)
            continue; // Canvas does not support combining clusters.
        if (width > end - x)
            break; // Never split a wide character at the edge.
        canvas.put(x, y, cp, fg, bg, bold);
        x += width;
    }
    return x;
}

// Blanks from column `x` to the end of the row in the given colours.
inline void fillRow(
    ::tty::Canvas &canvas, int y, ::tty::Rgb fg, ::tty::Rgb bg, int x = 0) {
    ::tty::Cell blank;
    blank.foreground = fg;
    blank.background = bg;
    for (; x < canvas.columns(); ++x) {
        canvas.put(x, y, blank);
    }
}

} // namespace frontend::ttymode
