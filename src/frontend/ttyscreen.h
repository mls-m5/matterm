#pragma once

// What `matterm --attach-tty` hands to the host terminal: a grid of cells with
// resolved colours and a cursor. The mode builds one per frame (the list, or
// the pane's screen) and gives it to whatever draws to the terminal. Plain
// data, no I/O, and nothing matterm-specific except FChar, so the drawing layer
// can be swapped (src/tty, docs/tasks/attach-tty.md).
#include "fchar.h"
#include "palette.h"
#include "renderframe.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace frontend::tty {

// Colours of the chrome the mode draws itself (list, hint line).
inline constexpr Rgb8 kScreenFg{220, 220, 220};
inline constexpr Rgb8 kScreenBg{0, 0, 0};

struct ScreenImage {
    int columns = 0;
    int rows = 0;
    std::vector<FChar>
        cells; // Row-major; colours are RGB (no palette references).
    int cursorColumn = 0;
    int cursorRow = 0;
    bool cursorVisible = false;

    ScreenImage() = default;
    ScreenImage(int columnCount, int rowCount)
        : columns(std::max(0, columnCount))
        , rows(std::max(0, rowCount))
        , cells(std::size_t(columns) * std::size_t(rows)) {
        FChar blank;
        blank.foreground = kScreenFg;
        blank.background = kScreenBg;
        std::fill(cells.begin(), cells.end(), blank);
    }

    bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < columns && y < rows;
    }
    FChar &at(int x, int y) {
        return cells[std::size_t(y) * std::size_t(columns) + std::size_t(x)];
    }
    const FChar &at(int x, int y) const {
        return cells[std::size_t(y) * std::size_t(columns) + std::size_t(x)];
    }
    bool operator==(const ScreenImage &other) const {
        if (columns != other.columns || rows != other.rows ||
            cursorVisible != other.cursorVisible) {
            return false;
        }
        if (cursorVisible && (cursorColumn != other.cursorColumn ||
                              cursorRow != other.cursorRow)) {
            return false;
        }
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const FChar &a = cells[i], &b = other.cells[i];
            if (a.codePoint != b.codePoint || a.attributes != b.attributes ||
                a.foreground.r != b.foreground.r ||
                a.foreground.g != b.foreground.g ||
                a.foreground.b != b.foreground.b ||
                a.background.r != b.background.r ||
                a.background.g != b.background.g ||
                a.background.b != b.background.b) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const ScreenImage &other) const {
        return !(*this == other);
    }
};

// UTF-8 to code points; a malformed byte becomes U+FFFD.
inline std::u32string decodeUtf8(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80         ? 1
                             : (lead >> 5) == 6  ? 2
                             : (lead >> 4) == 14 ? 3
                             : (lead >> 3) == 30 ? 4
                                                 : 0;
        if (length == 0 || i + length > text.size()) {
            out.push_back(U'�');
            ++i;
            continue;
        }
        char32_t cp =
            length == 1 ? lead : char32_t(lead & (0xffu >> (length + 1)));
        bool ok = true;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3fu);
        }
        if (!ok) {
            out.push_back(U'�');
            ++i;
            continue;
        }
        out.push_back(cp);
        i += length;
    }
    return out;
}

// Writes `text` from (x, y), clipped to the row. Returns the column after the
// last cell.
inline int put(ScreenImage &image,
               int x,
               int y,
               std::string_view text,
               Rgb8 fg = kScreenFg,
               Rgb8 bg = kScreenBg,
               CellAttribute attributes = CellAttribute::None) {
    if (y < 0 || y >= image.rows) {
        return x;
    }
    for (const char32_t cp : decodeUtf8(text)) {
        if (x >= image.columns) {
            break;
        }
        if (x >= 0) {
            FChar &cell = image.at(x, y);
            cell.codePoint = cp < 32 || cp == 127 ? U' ' : cp;
            cell.foreground = fg;
            cell.background = bg;
            cell.attributes = attributes;
        }
        ++x;
    }
    return x;
}

// Fills the row with `bg` (and blanks) from column `x` to the end.
inline void fillRow(ScreenImage &image, int y, Rgb8 fg, Rgb8 bg, int x = 0) {
    for (; x < image.columns && y >= 0 && y < image.rows; ++x) {
        FChar &cell = image.at(x, y);
        cell.codePoint = U' ';
        cell.foreground = fg;
        cell.background = bg;
        cell.attributes = CellAttribute::None;
    }
}

// The screen of a terminal pane as the host terminal shows it: the frame's
// cells with the palette applied (inverse, faint and conceal turn into colours;
// the cell keeps bold, italic, underline and strike), cut or padded to
// `columns` x `rows`.
inline ScreenImage imageOf(const RenderFrame &frame,
                           const Palette &palette,
                           int columns,
                           int rows) {
    ScreenImage image(columns, rows);
    const int shownRows = int(std::min<std::size_t>(
        frame.cells.rows(), std::size_t(std::max(0, rows))));
    const int shownColumns = int(std::min<std::size_t>(
        frame.cells.columns(), std::size_t(std::max(0, columns))));
    for (int y = 0; y < shownRows; ++y) {
        const FStringView row = frame.cells.row(std::size_t(y));
        for (int x = 0; x < shownColumns; ++x) {
            const FChar &source = row.data()[x];
            const ResolvedColors colors = palette.resolve(source);
            FChar &cell = image.at(x, y);
            cell.codePoint = source.codePoint < 32 || source.codePoint == 127
                                 ? U' '
                                 : source.codePoint;
            cell.foreground = colors.foreground;
            cell.background = colors.background;
            cell.attributes = CellAttribute(
                std::uint16_t(source.attributes) &
                std::uint16_t(CellAttribute::Bold | CellAttribute::Italic |
                              CellAttribute::Underline |
                              CellAttribute::Strike));
        }
    }
    image.cursorVisible = frame.cursor.visible && frame.cursor.column >= 0 &&
                          frame.cursor.row >= 0 &&
                          frame.cursor.column < columns &&
                          frame.cursor.row < rows;
    image.cursorColumn = frame.cursor.column;
    image.cursorRow = frame.cursor.row;
    return image;
}

} // namespace frontend::tty
