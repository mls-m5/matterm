#include "cellbuffer.h"
#include "mls-unit-test/unittest.h"
#include "palette.h"
#include "terminalmodel.h"
#include "tty/matterm/cells.h"
#include "vtparser.h"

using namespace tty;

namespace {
FChar plain(char32_t cp) {
    FChar cell;
    cell.codePoint = cp;
    return cell;
}
void expectSame(const FChar &a, const FChar &b) {
    EXPECT(a.codePoint == b.codePoint);
    EXPECT(a.style() == b.style());
    EXPECT(a.foregroundColor().kind == b.foregroundColor().kind);
    EXPECT(a.backgroundColor().kind == b.backgroundColor().kind);
    if (a.foregroundColor().kind != ColorReference::Kind::Default) {
        EXPECT(a.foreground.r == b.foreground.r);
        if (a.foregroundColor().kind == ColorReference::Kind::Rgb) {
            EXPECT(a.foreground.g == b.foreground.g &&
                   a.foreground.b == b.foreground.b);
        }
    }
    if (a.backgroundColor().kind != ColorReference::Kind::Default) {
        EXPECT(a.background.r == b.background.r);
        if (a.backgroundColor().kind == ColorReference::Kind::Rgb) {
            EXPECT(a.background.g == b.background.g &&
                   a.background.b == b.background.b);
        }
    }
}
} // namespace

TEST_SUIT_BEGIN(TtyMatterm)

TEST_CASE("A cell buffer drawn to a terminal arrives as it was") {
    CellBuffer buffer(8, 3);
    Palette palette;
    buffer.at(0, 0) = plain(U'a');
    FChar styled = plain(U'b');
    styled.setStyle(CellAttribute::Bold | CellAttribute::Underline |
                    CellAttribute::Italic | CellAttribute::Inverse);
    styled.setForeground(ColorReference::rgb({10, 200, 30}));
    styled.setBackground(ColorReference::rgb({1, 2, 3}));
    buffer.at(1, 0) = styled;
    FChar indexed = plain(U'c');
    indexed.setForeground(ColorReference::indexed(1));
    indexed.setBackground(ColorReference::indexed(220));
    buffer.at(2, 1) = indexed;
    FChar defaults = plain(U'd');
    defaults.setForeground(ColorReference::defaultColor());
    defaults.setBackground(ColorReference::defaultColor());
    defaults.setStyle(CellAttribute::Strike | CellAttribute::Faint);
    buffer.at(7, 2) = defaults;

    Screen screen(8, 3);
    TerminalModel host(8, 3);
    VtParser parser(host);
    parser.feed(screen.enter());
    drawCells(screen, buffer.view(), palette);
    parser.feed(screen.present());
    for (std::size_t y = 0; y < 3; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            expectSame(buffer.at(x, y), host.cell(x, y));
        }
    }
}

TEST_CASE("A NUL cell is a blank, not a missing column") {
    CellBuffer buffer(3, 1);
    buffer.at(0, 0) = plain(U'a');
    buffer.at(1, 0) = plain(U'\0');
    buffer.at(2, 0) = plain(U'c');
    Screen screen(3, 1);
    TerminalModel host(3, 1);
    VtParser parser(host);
    parser.feed(screen.enter());
    drawCells(screen, buffer.view(), Palette());
    parser.feed(screen.present());
    EXPECT(host.cell(1, 0).codePoint == U' ');
    EXPECT(host.cell(2, 0).codePoint == U'c');
}

TEST_CASE("The grid is clipped to the screen and may sit at an offset") {
    CellBuffer buffer(6, 4);
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 6; ++x) {
            buffer.at(x, y) = plain(U'a' + char32_t(y * 6 + x));
        }
    }
    Screen screen(4, 2);
    drawCells(screen, buffer.view(), Palette());
    EXPECT(screen.canvas().at(3, 1).codePoint == buffer.at(3, 1).codePoint);
    Screen offset(4, 2);
    drawCells(offset, buffer.view(), Palette(), 2, 1);
    EXPECT(offset.canvas().at(2, 1).codePoint == buffer.at(0, 0).codePoint);
    EXPECT(offset.canvas().at(3, 1).codePoint == buffer.at(1, 0).codePoint);
}

TEST_CASE("The cursor state becomes a host cursor") {
    CursorState state;
    state.column = 4;
    state.row = 2;
    state.visible = true;
    state.shape = CursorState::Shape::Bar;
    state.blink = false;
    auto cursor = toCursor(state);
    EXPECT(cursor.visible && cursor.column == 4 && cursor.row == 2);
    EXPECT(cursor.shape == Cursor::Shape::Bar && !cursor.blink);
    state.shape = CursorState::Shape::Block;
    EXPECT(toCursor(state).shape == Cursor::Shape::Block);
    state.shape = CursorState::Shape::Underline;
    EXPECT(toCursor(state).shape == Cursor::Shape::Underline);
    state.visible = false;
    EXPECT(!toCursor(state).visible);
}

TEST_SUIT_END
