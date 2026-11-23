#include "tty/matterm/cells.h"

#include <algorithm>

namespace tty {
namespace {
Rgb rgb(Rgb8 value) {
    return {value.r, value.g, value.b};
}
} // namespace

void drawCells(Screen &screen,
               const CellGridView &grid,
               const Palette &palette,
               int originX,
               int originY) {
    const int rows = int(grid.rows());
    const int columns = int(grid.columns());
    for (int y = std::max(0, -originY); y < rows && y + originY < screen.rows();
         ++y) {
        const auto row = grid.row(std::size_t(y));
        for (int x = std::max(0, -originX);
             x < columns && x + originX < screen.columns();
             ++x) {
            const FChar &from = row.data()[x];
            const auto foreground = from.foregroundColor();
            const auto background = from.backgroundColor();
            Cell cell;
            cell.codePoint = from.codePoint == 0 ? U' ' : from.codePoint;
            // The colours as stored: inverse, faint and conceal are attributes
            // for the host terminal to apply, not something to bake in.
            cell.foreground = rgb(palette.resolve(foreground, true));
            cell.background = rgb(palette.resolve(background, false));
            cell.defaultForeground =
                foreground.kind == ColorReference::Kind::Default;
            cell.defaultBackground =
                background.kind == ColorReference::Kind::Default;
            if (foreground.kind == ColorReference::Kind::Indexed) {
                cell.foregroundIndex = foreground.value.r;
            }
            if (background.kind == ColorReference::Kind::Indexed) {
                cell.backgroundIndex = background.value.r;
            }
            const auto style = from.style();
            cell.bold = hasAttribute(style, CellAttribute::Bold);
            cell.italic = hasAttribute(style, CellAttribute::Italic);
            cell.underline = hasAttribute(style, CellAttribute::Underline);
            cell.strike = hasAttribute(style, CellAttribute::Strike);
            cell.faint = hasAttribute(style, CellAttribute::Faint);
            cell.blink = hasAttribute(style, CellAttribute::Blink);
            cell.inverse = hasAttribute(style, CellAttribute::Inverse);
            cell.conceal = hasAttribute(style, CellAttribute::Conceal);
            screen.canvas().put(x + originX, y + originY, cell);
        }
    }
}

Cursor toCursor(const CursorState &state, int originX, int originY) {
    Cursor cursor;
    cursor.visible = state.visible;
    cursor.column = state.column + originX;
    cursor.row = state.row + originY;
    cursor.blink = state.blink;
    switch (state.shape) {
    case CursorState::Shape::Block:
        cursor.shape = Cursor::Shape::Block;
        break;
    case CursorState::Shape::Underline:
        cursor.shape = Cursor::Shape::Underline;
        break;
    case CursorState::Shape::Bar:
        cursor.shape = Cursor::Shape::Bar;
        break;
    }
    return cursor;
}

} // namespace tty
