#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <string>

namespace {
void filled(TerminalModel &model) {
    for (std::size_t y = 0; y < model.rows(); ++y) {
        model.moveCursor(0, int(y));
        model.write(std::u32string(model.columns(), char32_t(U'A' + y)));
    }
}
} // namespace

TEST_SUIT_BEGIN(TerminalOperations)

TEST_CASE("Scrolling margins preserve outside rows and partial scrolls never "
          "enter history") {
    TerminalModel model(5, 6, 10);
    filled(model);
    EXPECT(model.setScrollRegion(1, 4));
    model.moveCursor(0, 4);
    model.lineFeed();
    EXPECT(model.cell(0, 0).codePoint == U'A');
    EXPECT(model.cell(0, 1).codePoint == U'C');
    EXPECT(model.cell(0, 3).codePoint == U'E');
    EXPECT(model.cell(0, 4).codePoint == U' ');
    EXPECT(model.cell(0, 5).codePoint == U'F');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.moveCursor(0, 1);
    model.reverseIndex();
    EXPECT(model.cell(0, 1).codePoint == U' ');
    EXPECT(model.cell(0, 2).codePoint == U'C');
    EXPECT(model.cell(0, 5).codePoint == U'F');
    model.setScrollRegion(0, 5);
    model.moveCursor(0, 5);
    model.lineFeed();
    EXPECT_EQ(model.historySize(), std::size_t(1));
}

TEST_CASE("Insert delete and erase characters move cells and use current blank "
          "colors") {
    TerminalModel model(6, 2);
    model.write(U"abcdef");
    model.moveCursor(2, 0);
    model.setColors({1, 2, 3}, {4, 5, 6});
    model.insertCharacters(2);
    EXPECT(model.cell(4, 0).codePoint == U'c');
    EXPECT(model.cell(2, 0).codePoint == U' ');
    EXPECT_EQ(model.cell(2, 0).background.b, 6);
    model.deleteCharacters(1);
    EXPECT(model.cell(3, 0).codePoint == U'c');
    EXPECT(model.cell(4, 0).codePoint == U'd');
    model.eraseCharacters(2);
    EXPECT(model.cell(3, 0).codePoint == U' ');
    EXPECT(model.cell(4, 0).codePoint == U'd');
    model.moveCursor(1, 0);
    model.setInsertMode(true);
    model.write(U"X");
    EXPECT(model.cell(1, 0).codePoint == U'X');
    EXPECT(model.cell(2, 0).codePoint == U'b');
    EXPECT(model.cell(5, 0).codePoint == U'd');
    model.deleteCharacters(999);
    EXPECT(model.cell(5, 0).codePoint == U' ');
}

TEST_CASE("Insert delete lines respect the cursor and bottom margin without "
          "history") {
    TerminalModel model(3, 6, 10);
    filled(model);
    model.setScrollRegion(1, 4);
    model.moveCursor(0, 2);
    model.insertLines();
    EXPECT(model.cell(0, 2).codePoint == U' ');
    EXPECT(model.cell(0, 3).codePoint == U'C');
    EXPECT(model.cell(0, 4).codePoint == U'D');
    model.deleteLines(2);
    EXPECT(model.cell(0, 2).codePoint == U'D');
    EXPECT(model.cell(0, 3).codePoint == U' ');
    EXPECT(model.cell(0, 5).codePoint == U'F');
    model.moveCursor(0, 0);
    model.insertLines(2);
    EXPECT(model.cell(0, 0).codePoint == U'A');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    model.setScrollRegion(0, 5);
    model.deleteLines(6);
    EXPECT_EQ(model.historySize(), std::size_t(0));
}

TEST_CASE(
    "Origin addressing and relative movement clamp to the scrolling region") {
    TerminalModel model(8, 6);
    std::string reply;
    VtParser parser(model,
                    {[&](std::string_view bytes) { reply = bytes; }, {}});
    parser.feed("\x1b[2;5r\x1b[?6h\x1b[2;3H\x1b[6n");
    EXPECT_EQ(model.cursor().row, 2);
    EXPECT_EQ(model.cursor().column, 2);
    EXPECT_EQ(reply, std::string("\x1b[2;3R"));
    parser.feed("\x1b[999B");
    EXPECT_EQ(model.cursor().row, 4);
    parser.feed("\x1b[999A");
    EXPECT_EQ(model.cursor().row, 1);
    parser.feed("\x1b[5;2r");
    EXPECT_EQ(model.scrollTop(), 1);
    parser.feed("\x1b[?6l\x1b[H");
    EXPECT_EQ(model.cursor().row, 0);
}

TEST_CASE(
    "Autowrap can be disabled and reenabled independently of insert mode") {
    TerminalModel model(3, 2);
    VtParser parser(model);
    parser.feed("\x1b[?7labcd");
    EXPECT(model.cell(2, 0).codePoint == U'd');
    EXPECT(model.cell(0, 1).codePoint == U' ');
    parser.feed("\x1b[?7hef");
    EXPECT(model.cell(2, 0).codePoint == U'e');
    EXPECT(model.cell(0, 1).codePoint == U'f');
    parser.feed("\x1b[H\x1b[4hX\x1b[4l");
    EXPECT(model.cell(0, 0).codePoint == U'X');
    EXPECT(model.cell(1, 0).codePoint == U'a');
}

TEST_CASE("DEC line graphics SI SO and cursor save restore select the proper "
          "mapping") {
    TerminalModel model(20, 2);
    VtParser parser(model);
    parser.feed("\x1b(0lqqk\x1b(Bq\x1b)0\x0e"
                "x\x0f"
                "x");
    const std::u32string expected = U"┌──┐q│x";
    for (std::size_t x = 0; x < expected.size(); ++x) {
        EXPECT(model.cell(x, 0).codePoint == expected[x]);
    }
    parser.feed("\x1b(0\x1b"
                "7\x1b(B\x1b"
                "8q");
    EXPECT(model.cell(7, 0).codePoint == U'─');
}

TEST_CASE("Inverse conceal and reset colors use logical rather than already "
          "swapped colors") {
    TerminalModel model(10, 2);
    VtParser parser(model, {}, {1, 2, 3}, {4, 5, 6});
    parser.feed("\x1b[7mA\x1b[27mB\x1b[8mC\x1b[28mD");
    EXPECT_EQ(model.cell(0, 0).foreground.r, 4);
    EXPECT_EQ(model.cell(0, 0).background.r, 1);
    EXPECT_EQ(model.cell(1, 0).foreground.r, 1);
    EXPECT_EQ(model.cell(2, 0).foreground.r, 4);
    EXPECT_EQ(model.cell(2, 0).background.r, 4);
    EXPECT_EQ(model.cell(3, 0).foreground.r, 1);
}

TEST_CASE("CSI insertion erasure scrolling and reverse index dispatch to model "
          "operations") {
    TerminalModel model(5, 4, 8);
    VtParser parser(model);
    parser.feed("abcde\x1b[1;2H\x1b[@X\x1b[P\x1b[X");
    EXPECT(model.cell(0, 0).codePoint == U'a');
    EXPECT(model.cell(1, 0).codePoint == U'X');
    EXPECT(model.cell(2, 0).codePoint == U' ');
    parser.feed("\x1b[2;1HBBBB\x1b[3;1HCCCC\x1b[2;1H\x1b[L");
    EXPECT(model.cell(0, 1).codePoint == U' ');
    EXPECT(model.cell(0, 2).codePoint == U'B');
    parser.feed("\x1b[M");
    EXPECT(model.cell(0, 1).codePoint == U'B');
    parser.feed("\x1b[H\x1bM");
    EXPECT(model.cell(0, 0).codePoint == U' ');
    parser.feed("\x1b[S");
    EXPECT_EQ(model.historySize(), std::size_t(1));
    parser.feed("\x1b[T");
    EXPECT(model.cell(0, 0).codePoint == U' ');
}

TEST_CASE("RIS resets modes buffers margins style and device identification "
          "has a reply channel") {
    TerminalModel model(8, 5);
    std::string reply;
    VtParser parser(model,
                    {[&](std::string_view bytes) { reply = bytes; }, {}});
    parser.feed(
        "\x1b[?1h\x1b[?2004h\x1b[?1049h\x1b[2;4r\x1b[?6h\x1b[7mhello\x1b"
        "cX");
    EXPECT_FALSE(model.alternateScreen());
    EXPECT_FALSE(model.originMode());
    EXPECT_FALSE(parser.modes().applicationCursor);
    EXPECT_FALSE(parser.modes().bracketedPaste);
    EXPECT_EQ(model.scrollTop(), 0);
    EXPECT_EQ(model.scrollBottom(), 4);
    EXPECT_EQ(model.historySize(), std::size_t(0));
    EXPECT(model.cell(0, 0).codePoint == U'X');
    parser.feed("\x1b[c");
    EXPECT_EQ(reply, std::string("\x1b[?1;0c"));
}

TEST_SUIT_END
