#include "cellhit.h"
#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

using frontend::cellAt;

TEST_SUIT_BEGIN(CellHit)

TEST_CASE("A point maps to the cell under it") {
    TerminalModel model(10, 3, 100);
    // 8x16 cells, drawn on a 100x60 surface.
    const auto first = cellAt(model, 0, 0, 8, 16, 100, 60, false);
    EXPECT(first.has_value());
    EXPECT_EQ(first->column, std::size_t(0));
    const auto middle = cellAt(model, 17, 20, 8, 16, 100, 60, false);
    EXPECT_EQ(middle->column, std::size_t(2));
    EXPECT_EQ(middle->row, first->row + 1);
    const auto last = cellAt(model, 79, 47, 8, 16, 100, 60, false);
    EXPECT_EQ(last->column, std::size_t(9));
    EXPECT_EQ(last->row, first->row + 2);
}

TEST_CASE("Outside the text there is no cell, unless clamping is asked for") {
    TerminalModel model(10, 3, 100);
    EXPECT(!cellAt(model, 80, 0, 8, 16, 100, 60, false).has_value());
    EXPECT(!cellAt(model, 0, 48, 8, 16, 100, 60, false).has_value());
    EXPECT(!cellAt(model, -1, 5, 8, 16, 100, 60, false).has_value());
    EXPECT(!cellAt(model, 5, -1, 8, 16, 100, 60, false).has_value());
    const auto clamped = cellAt(model, 500, 500, 8, 16, 100, 60, true);
    EXPECT_EQ(clamped->column, std::size_t(9));
    const auto before = cellAt(model, -50, -50, 8, 16, 100, 60, true);
    EXPECT_EQ(before->column, std::size_t(0));
}

TEST_CASE("A pane narrower than the text limits the cells") {
    TerminalModel model(10, 3, 100);
    // The surface is only 40 pixels wide: five columns fit.
    EXPECT(cellAt(model, 39, 0, 8, 16, 40, 60, false).has_value());
    EXPECT(!cellAt(model, 40, 0, 8, 16, 40, 60, false).has_value());
    EXPECT_EQ(cellAt(model, 500, 0, 8, 16, 40, 60, true)->column,
              std::size_t(4));
}

TEST_CASE("An empty surface or cell size has no cells") {
    TerminalModel model(10, 3, 100);
    EXPECT(!cellAt(model, 0, 0, 8, 16, 0, 60, true).has_value());
    EXPECT(!cellAt(model, 0, 0, 8, 16, 100, 0, true).has_value());
}

TEST_CASE("Rows follow the scrolled history") {
    TerminalModel model(10, 3, 100);
    VtParser parser(model);
    for (int i = 0; i < 20; ++i) {
        parser.feed("line\r\n");
    }
    const auto bottom = cellAt(model, 0, 0, 8, 16, 100, 60, false);
    model.scrollBy(5);
    model.advanceViewport(10);
    const auto scrolled = cellAt(model, 0, 0, 8, 16, 100, 60, false);
    EXPECT(scrolled->row + 5 <= bottom->row + 1);
    EXPECT(scrolled->row < bottom->row);
}

TEST_SUIT_END
