#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace fakeos {

struct Cell {
    char32_t codePoint = U' ';
    Rgb foreground{200, 200, 200};
    Rgb background{0, 0, 0};
    bool bold = false;
    friend bool operator==(const Cell &a, const Cell &b) {
        return a.codePoint == b.codePoint && a.foreground == b.foreground &&
               a.background == b.background && a.bold == b.bold;
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
class AsciiCanvas {
public:
    AsciiCanvas(int columns, int rows, Capabilities caps = {});

    // Changes the size; the next flush redraws everything.
    void resize(int columns, int rows);
    void setCapabilities(Capabilities caps);
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
    void appendColor(std::string &out, bool foreground, Rgb color) const;

    int columns_, rows_;
    Capabilities caps_;
    std::vector<Cell> cells_, shown_;
    bool known_ = false;
};

// Quantizing helpers, exposed for tests.
int nearestXterm256(Rgb color);
int nearestAnsi16(Rgb color);
void appendUtf8(std::string &out, char32_t codePoint);
std::u32string decodeUtf8(const std::string &text);

} // namespace fakeos
