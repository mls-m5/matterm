#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"

#include <limits>
#include <stdexcept>

TEST_SUIT_BEGIN(TerminalModel)

TEST_CASE("Writing uses deferred wrap and explicit CR LF semantics") {
    TerminalModel model(3, 2, 4);
    model.write(U"abc");
    EXPECT_EQ(model.cursor().column, 2);
    EXPECT_EQ(model.cursor().row, 0);
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.write(U"d");
    EXPECT(model.cell(0, 1).codePoint == U'd');
    EXPECT_EQ(model.cursor().column, 1);
    model.write(U"\ne"); // LF preserves column and scrolls at the bottom.
    EXPECT(model.cell(0, 0).codePoint == U'd');
    EXPECT(model.cell(1, 1).codePoint == U'e');
    EXPECT_EQ(model.historySize(), std::size_t(1));
    model.write(U"\rX");
    EXPECT(model.cell(0, 1).codePoint == U'X');
}

TEST_CASE(
    "Exact-width text followed by CR LF does not add an extra wrapped line") {
    TerminalModel model(2, 2, 4);
    model.write(U"ab\r\ncd");
    EXPECT(model.cell(0, 0).codePoint == U'a');
    EXPECT(model.cell(0, 1).codePoint == U'c');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.write(U"e");
    EXPECT_EQ(model.historySize(), std::size_t(1));
    EXPECT(model.cell(0, 0).codePoint == U'c');
    EXPECT(model.cell(0, 1).codePoint == U'e');
}

TEST_CASE("Cursor clipping tabs backspace and invalid code points are safe") {
    TerminalModel model(12, 2);
    model.write(U"a\tb\bC");
    EXPECT(model.cell(8, 0).codePoint == U'C');
    model.moveCursor(-100, 100);
    EXPECT_EQ(model.cursor().column, 0);
    EXPECT_EQ(model.cursor().row, 1);
    model.write(std::u32string_view(U"\x1b\x7f\x85"));
    EXPECT_EQ(model.cursor().column, 0);
    const char32_t invalid[]{char32_t(0xd800), char32_t(0x110000)};
    model.write(std::u32string_view(invalid, 2));
    EXPECT(model.cell(0, 1).codePoint == U'\ufffd');
    EXPECT(model.cell(1, 1).codePoint == U'\ufffd');
    EXPECT_THROW(model.cell(12, 0), std::out_of_range);
    EXPECT_THROW(model.cell(0, 2), std::out_of_range);
}

TEST_CASE(
    "Erasing is inclusive uses current colors and leaves the cursor in place") {
    TerminalModel model(4, 3, 4);
    model.write(U"abcd\r\nefgh\r\nijkl");
    model.setColors({1, 2, 3}, {4, 5, 6});
    model.moveCursor(1, 1);
    model.eraseLine(TerminalModel::EraseMode::ToStart);
    EXPECT(model.cell(0, 1).codePoint == U' ');
    EXPECT(model.cell(1, 1).codePoint == U' ');
    EXPECT(model.cell(2, 1).codePoint == U'g');
    EXPECT_EQ(model.cell(0, 1).background.b, 6);
    model.eraseDisplay();
    EXPECT(model.cell(0, 0).codePoint == U'a');
    EXPECT(model.cell(3, 1).codePoint == U' ');
    EXPECT(model.cell(0, 2).codePoint == U' ');
    EXPECT_EQ(model.cursor().column, 1);
    EXPECT_EQ(model.cursor().row, 1);
    model.eraseDisplay(TerminalModel::EraseMode::All);
    EXPECT(model.cell(0, 0).codePoint == U' ');
}

TEST_CASE(
    "Bounded history remains readable after repeated full-screen scrolls") {
    TerminalModel model(2, 2, 3);
    for (char32_t ch = U'a'; ch <= U'j'; ++ch) {
        model.write(std::u32string_view(&ch, 1));
        model.write(U"\r\n");
    }
    EXPECT_EQ(model.historySize(), std::size_t(3));
    EXPECT(model.cell(0, 0).codePoint == U'j');
    model.scrollToOldest();
    model.advanceViewport(10);
    const auto frame = model.frame(20);
    EXPECT(frame.cells.row(0).data()[0].codePoint == U'g');
    EXPECT_FALSE(frame.cursor.visible);
    EXPECT(frame.scrollbar.visible);
    model.eraseDisplay(TerminalModel::EraseMode::All);
    const auto afterErase = model.frame(20);
    EXPECT(afterErase.cells.row(0).data()[0].codePoint == U'g');
}

TEST_CASE("Alternate screen isolates text history style cursor and viewport") {
    TerminalModel model(4, 2, 5);
    model.write(U"one\r\ntwo\r\nthree\r\n");
    model.moveCursor(1, 1);
    model.setColors({10, 20, 30}, {40, 50, 60});
    model.scrollBy(0.5);
    model.advanceViewport(10);
    const auto oldOffset = model.viewport().offset();
    const auto oldHistory = model.historySize();
    const auto oldCell = model.cell(0, 0).codePoint;
    model.setAlternateScreen(true);
    EXPECT(model.alternateScreen());
    EXPECT_EQ(model.historySize(), std::size_t(0));
    EXPECT_EQ(model.cursor().column, 0);
    EXPECT(model.cell(0, 0).codePoint == U' ');
    EXPECT_FALSE(model.frame(20).scrollbar.visible);
    model.write(U"ALT");
    model.setAlternateScreen(true); // Re-entry is a no-op.
    EXPECT(model.cell(0, 0).codePoint == U'A');
    model.setColors({200, 0, 0}, {0, 0, 0});
    for (int i = 0; i < 50; ++i) {
        model.write(U"x\r\n");
    }
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.scrollToOldest();
    model.advanceViewport(10);
    EXPECT(model.viewport().atTail());
    model.setAlternateScreen(false);
    EXPECT_EQ(model.historySize(), oldHistory);
    EXPECT_EQ(model.viewport().offset(), oldOffset);
    EXPECT_EQ(model.cursor().column, 1);
    EXPECT_EQ(model.cursor().row, 1);
    EXPECT(model.cell(0, 0).codePoint == oldCell);
    model.write(U"Z");
    EXPECT_EQ(model.cell(1, 1).foreground.g, 20);
    EXPECT_EQ(model.cell(1, 1).background.b, 60);
    model.setAlternateScreen(true);
    EXPECT(model.cell(0, 0).codePoint == U' ');
}

TEST_CASE("Resize preserves both buffers clips width and keeps bottom rows on "
          "shrink") {
    TerminalModel model(4, 3, 5);
    model.write(U"aaaa\r\nbbbb\r\ncccc");
    model.setAlternateScreen(true);
    model.write(U"AAAA\r\nBBBB\r\nCCCC");
    model.resize(3, 2);
    EXPECT(model.cell(0, 0).codePoint == U'B');
    EXPECT(model.cell(2, 1).codePoint == U'C');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.setAlternateScreen(false);
    EXPECT(model.cell(0, 0).codePoint == U'b');
    EXPECT(model.cell(2, 1).codePoint == U'c');
    EXPECT_EQ(model.historySize(), std::size_t(1));
    EXPECT_EQ(model.cursor().row, 1);
    EXPECT_EQ(model.cursor().column, 2);
    model.resize(5, 4);
    EXPECT(model.cell(0, 0).codePoint == U'b');
    EXPECT(model.cell(3, 0).codePoint == U' ');
    EXPECT(model.cell(0, 3).codePoint == U' ');
    EXPECT_EQ(model.historySize(), std::size_t(1));
}

TEST_CASE("Switching screens preserves a pending primary wrap") {
    TerminalModel model(2, 2, 2);
    model.write(U"ab");
    model.setAlternateScreen(true);
    model.write(U"X");
    model.setAlternateScreen(false);
    model.write(U"c");
    EXPECT(model.cell(0, 0).codePoint == U'a');
    EXPECT(model.cell(1, 0).codePoint == U'b');
    EXPECT(model.cell(0, 1).codePoint == U'c');
}

TEST_CASE("Resize retains the insertion position after a full-width write") {
    TerminalModel model(3, 2, 2);
    model.write(U"abc");
    model.resize(5, 2);
    model.write(U"X");
    EXPECT(model.cell(2, 0).codePoint == U'c');
    EXPECT(model.cell(3, 0).codePoint == U'X');
    model.write(U"Y");
    model.resize(4, 2);
    model.write(U"Z");
    EXPECT(model.cell(0, 1).codePoint == U'Z');
}

TEST_CASE("Single-cell screens and zero scrollback handle wrap and erasure") {
    TerminalModel model(1, 1, 0);
    model.write(U"ab");
    EXPECT(model.cell(0, 0).codePoint == U'b');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.eraseLine();
    model.write(U"c");
    EXPECT(model.cell(0, 0).codePoint == U'c');
    model.write(U"\b\td");
    EXPECT(model.cell(0, 0).codePoint == U'd');
}

TEST_CASE("Invalid sizes fail without changing either screen") {
    EXPECT_THROW((TerminalModel(0, 1)), std::invalid_argument);
    EXPECT_THROW((TerminalModel(1, 0)), std::invalid_argument);
    TerminalModel model(4, 2, 3);
    model.write(U"keep");
    EXPECT_THROW(model.resize(0, 3), std::invalid_argument);
    EXPECT_THROW(model.resize(std::numeric_limits<std::size_t>::max(), 3),
                 std::invalid_argument);
    EXPECT_THROW(model.resize(std::size_t(std::numeric_limits<int>::max()),
                              std::size_t(std::numeric_limits<int>::max())),
                 std::length_error);
    EXPECT(model.cell(0, 0).codePoint == U'k');
    EXPECT_EQ(model.columns(), std::size_t(4));
    EXPECT_THROW(model.frame(0), std::invalid_argument);
    EXPECT_THROW(model.frame(std::numeric_limits<int>::max()),
                 std::invalid_argument);
}

TEST_CASE("Saved cursor is per screen and safely clamps after resizing") {
    TerminalModel model(5, 3, 3);
    model.moveCursor(4, 2);
    model.setColors({1, 2, 3}, {4, 5, 6});
    model.saveCursor();
    model.setAlternateScreen(true);
    model.moveCursor(1, 1);
    model.saveCursor();
    model.moveCursor(0, 0);
    model.restoreCursor();
    EXPECT_EQ(model.cursor().column, 1);
    EXPECT_EQ(model.cursor().row, 1);
    model.setAlternateScreen(false);
    model.resize(2, 1);
    model.restoreCursor();
    EXPECT_EQ(model.cursor().column, 1);
    EXPECT_EQ(model.cursor().row, 0);
    model.write(U"X");
    EXPECT_EQ(model.cell(1, 0).foreground.g, 2);
    EXPECT_EQ(model.cell(1, 0).background.b, 6);
}

TEST_CASE("Changing cursor visibility does not cancel pending wrap") {
    TerminalModel model(2, 2);
    model.write(U"ab");
    model.setCursorVisible(false);
    model.write(U"c");
    EXPECT(model.cell(0, 1).codePoint == U'c');
    EXPECT_FALSE(model.cursor().visible);
}

TEST_CASE(
    "Selection copies UTF-8 across scrollback and joins only soft wraps") {
    TerminalModel model(4, 2, 2);
    model.write(U"abcd\u2801\U0001f642\r\nnext");
    const auto first = model.firstRetainedRow();
    const auto selected = model.copyText({first + 2, 2}, {first, 1}, 32);
    EXPECT(selected.has_value());
    if (selected) {
        EXPECT_EQ(*selected,
                  std::string("bcd\xe2\xa0\x81\xf0\x9f\x99\x82\nnex"));
    }
    EXPECT(!model.copyText({first, 1}, {first + 2, 2}, 7));
    const auto epoch = model.selectionEpoch();
    model.write(U"\r\na\r\nb");
    EXPECT(model.firstRetainedRow() > first);
    EXPECT_EQ(model.selectionEpoch(), epoch);
    EXPECT(!model.copyText({first, 1}, {first + 2, 2}, 32));
    model.setAlternateScreen(true);
    EXPECT(model.selectionEpoch() != epoch);
}

TEST_CASE("A full line followed by an explicit newline is not a soft wrap") {
    TerminalModel model(4, 2);
    model.write(U"abcd\r\nef");
    const auto selected = model.copyText({0, 0}, {1, 1}, 7);
    EXPECT(selected.has_value());
    if (selected) {
        EXPECT_EQ(*selected, std::string("abcd\nef"));
    }
}

TEST_SUIT_END
