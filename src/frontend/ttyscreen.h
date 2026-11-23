#pragma once

// Small helpers for drawing the mode's own screens (the list, the hint line)
// into the shared terminal layer's canvas (src/tty). The pane itself is drawn
// by tty::drawCells.
#include "tty/canvas.h"
#include "tty/utf8.h"

#include <string>

namespace frontend::ttymode {

inline constexpr ::tty::Rgb kScreenFg{220, 220, 220};
inline constexpr ::tty::Rgb kScreenBg{0, 0, 0};

// Writes `text` from (x, y), clipped to the row, with control characters shown
// as blanks. Returns the column after the last character.
inline int put(::tty::Canvas &canvas,
               int x,
               int y,
               const std::string &text,
               ::tty::Rgb fg = kScreenFg,
               ::tty::Rgb bg = kScreenBg,
               bool bold = false) {
    std::string clean = text;
    for (char &c : clean) {
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
            c = ' ';
        }
    }
    canvas.text(x, y, clean, fg, bg, bold);
    return x + int(::tty::decodeUtf8(clean).size());
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
