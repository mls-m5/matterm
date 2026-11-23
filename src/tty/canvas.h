#pragma once

#include "tty/color.h"

#include <string>
#include <vector>

namespace tty {

struct Cell {
    char32_t codePoint = U' ';
    Rgb foreground{200, 200, 200};
    Rgb background{0, 0, 0};
    bool bold = false;
    // More attributes; all off by default, so Cell{cp, fg, bg, bold} is as
    // before.
    bool italic = false, underline = false, strike = false, faint = false;
    bool blink = false, inverse = false, conceal = false;
    // The terminal's own colours: when set, the RGB value is not used.
    bool defaultForeground = false, defaultBackground = false;
    // A palette index (0..255) to pass through, so the terminal's theme picks
    // the colour for 0..15; -1: use the RGB value. The RGB value stays the
    // fallback where the terminal has no 256-colour mode.
    int foregroundIndex = -1, backgroundIndex = -1;
    friend bool operator==(const Cell &a, const Cell &b) {
        return a.codePoint == b.codePoint && a.sameStyle(b);
    }
    // Everything but the character.
    bool sameStyle(const Cell &o) const {
        return foreground == o.foreground && background == o.background &&
               bold == o.bold && italic == o.italic &&
               underline == o.underline && strike == o.strike &&
               faint == o.faint && blink == o.blink && inverse == o.inverse &&
               conceal == o.conceal &&
               defaultForeground == o.defaultForeground &&
               defaultBackground == o.defaultBackground &&
               foregroundIndex == o.foregroundIndex &&
               backgroundIndex == o.backgroundIndex;
    }
    friend bool operator!=(const Cell &a, const Cell &b) {
        return !(a == b);
    }
};

// A cell buffer that turns drawing into the smallest VT update. flush()
// emits only the cells that changed since the previous flush (cursor
// addressing plus SGR, colors reduced to what the terminal supports), so
// animations do not flicker and stay light on a stream. An unchanged canvas
// flushes to the empty string.
class Canvas {
public:
    Canvas(int columns, int rows, Caps caps = {});

    // Changes the size; the next flush redraws everything.
    void resize(int columns, int rows);
    void setCaps(Caps caps);
    // Treat double-width characters as two columns (the cell after one is
    // covered and not drawn) and zero-width ones as a blank. Off by default:
    // every cell is then one column, as the pretend system's programs expect.
    void setWideChars(bool on);
    // For a terminal that is not UTF-8: every non-ASCII character is drawn as
    // '?' (one column).
    void setAsciiOnly(bool on);
    int columns() const {
        return columns_;
    }
    int rows() const {
        return rows_;
    }

    void clear(Rgb background = {0, 0, 0});
    // Out of range positions are ignored.
    void put(int x, int y, Cell cell);
    void put(int x,
             int y,
             char32_t codePoint,
             Rgb foreground,
             Rgb background,
             bool bold = false);
    // ASCII/UTF-8 text from (x, y), clipped to the row.
    void text(int x,
              int y,
              const std::string &utf8,
              Rgb foreground,
              Rgb background = {0, 0, 0},
              bool bold = false);
    const Cell &at(int x, int y) const;
    // The run of non-blank characters around a cell, without surrounding
    // punctuation like ( ) [ ]: the text that was drawn where a click landed.
    // Empty when the cell is blank or outside.
    std::string wordAt(int x, int y) const;

    // Updates since the last flush; `forceFull` redraws every cell.
    std::string flush(bool forceFull = false);
    // The next flush redraws everything (after the screen was cleared or
    // switched by something else).
    void invalidate() {
        known_ = false;
    }

private:
    int columns_, rows_;
    Caps caps_;
    std::vector<Cell> cells_, shown_;
    bool known_ = false;
    bool wide_ = false;
    bool asciiOnly_ = false;
};

} // namespace tty
