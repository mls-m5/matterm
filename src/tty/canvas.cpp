#include "tty/canvas.h"

#include "tty/utf8.h"
#include "tty/width.h"

#include <algorithm>

namespace tty {

Canvas::Canvas(int columns, int rows, Caps caps)
    : columns_(std::max(1, columns))
    , rows_(std::max(1, rows))
    , caps_(caps)
    , cells_(std::size_t(columns_) * std::size_t(rows_))
    , shown_(cells_.size()) {}

void Canvas::resize(int columns, int rows) {
    columns_ = std::max(1, columns);
    rows_ = std::max(1, rows);
    cells_.assign(std::size_t(columns_) * std::size_t(rows_), Cell{});
    shown_ = cells_;
    known_ = false;
}

void Canvas::setWideChars(bool on) {
    wide_ = on;
    known_ = false;
}

void Canvas::setAsciiOnly(bool on) {
    asciiOnly_ = on;
    known_ = false;
}

void Canvas::setCaps(Caps caps) {
    caps_ = caps;
    known_ = false;
}

void Canvas::clear(Rgb background) {
    Cell blank;
    blank.background = background;
    std::fill(cells_.begin(), cells_.end(), blank);
}

void Canvas::put(int x, int y, Cell cell) {
    if (x >= 0 && x < columns_ && y >= 0 && y < rows_) {
        cells_[std::size_t(y) * std::size_t(columns_) + std::size_t(x)] = cell;
    }
}

void Canvas::put(int x, int y, char32_t cp, Rgb fg, Rgb bg, bool bold) {
    put(x, y, Cell{cp, fg, bg, bold});
}

void Canvas::text(
    int x, int y, const std::string &utf8, Rgb fg, Rgb bg, bool bold) {
    for (const auto cp : decodeUtf8(utf8)) {
        put(x++, y, cp, fg, bg, bold);
    }
}

const Cell &Canvas::at(int x, int y) const {
    return cells_.at(std::size_t(y) * std::size_t(columns_) + std::size_t(x));
}

std::string Canvas::wordAt(int x, int y) const {
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

namespace {
// The SGR parameters for one cell (after the reset), in a fixed order.
void appendStyle(std::string &out, const Cell &cell, Caps caps) {
    out += "\x1b[0";
    if (cell.bold) {
        out += ";1";
    }
    if (cell.faint) {
        out += ";2";
    }
    if (cell.italic) {
        out += ";3";
    }
    if (cell.underline) {
        out += ";4";
    }
    if (cell.blink) {
        out += ";5";
    }
    if (cell.inverse) {
        out += ";7";
    }
    if (cell.conceal) {
        out += ";8";
    }
    if (cell.strike) {
        out += ";9";
    }
    out += ';';
    appendSgrColor(out,
                   true,
                   cell.foreground,
                   cell.defaultForeground,
                   cell.foregroundIndex,
                   caps);
    out += ';';
    appendSgrColor(out,
                   false,
                   cell.background,
                   cell.defaultBackground,
                   cell.backgroundIndex,
                   caps);
    out += 'm';
}
} // namespace

std::string Canvas::flush(bool forceFull) {
    std::string out;
    const bool full = forceFull || !known_;
    int cursorX = -1, cursorY = -1;
    bool haveStyle = false;
    Cell style;
    for (int y = 0; y < rows_; ++y) {
        int coveredUntil = 0; // Columns before this are under a wide character.
        for (int x = 0; x < columns_; ++x) {
            const auto index =
                std::size_t(y) * std::size_t(columns_) + std::size_t(x);
            const Cell &cell = cells_[index];
            int width = 1;
            char32_t shown = cell.codePoint < 0x20 ? U' ' : cell.codePoint;
            if (asciiOnly_ && shown >= 0x80) {
                shown = U'?';
            }
            else if (wide_) {
                width = cellWidth(cell.codePoint);
                if (width == 0) {
                    width = 1;
                    shown = U' ';
                }
                else if (width == 2 && x + 1 >= columns_) {
                    width = 1; // No room: a blank rather than a split.
                    shown = U' ';
                }
            }
            if (x < coveredUntil) {
                continue; // The wide character before it covers this cell.
            }
            coveredUntil = x + width;
            // A narrow character over a wide one leaves the covered column
            // blank on the terminal: draw that cell too.
            const bool uncovered =
                wide_ && x > 0 && cellWidth(shown_[index - 1].codePoint) == 2 &&
                cellWidth(cells_[index - 1].codePoint) != 2;
            if (!full && !uncovered && cell == shown_[index]) {
                continue;
            }
            if (cursorX != x || cursorY != y) {
                out += "\x1b[" + std::to_string(y + 1) + ";" +
                       std::to_string(x + 1) + "H";
            }
            if (!haveStyle || !cell.sameStyle(style)) {
                appendStyle(out, cell, caps_);
                style = cell;
                haveStyle = true;
            }
            appendUtf8(out, shown);
            cursorX = x + width;
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

} // namespace tty
