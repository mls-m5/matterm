#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include "palette.h"

#include <optional>
#include <string>
#include <vector>

namespace {
void filled(TerminalModel &model) {
    for (std::size_t y = 0; y < model.rows(); ++y) {
        model.moveCursor(0, int(y));
        model.write(std::u32string(model.columns(), char32_t(U'A' + y)));
    }
}
// All retained text, history first, one line per row.
std::string everything(const TerminalModel &model) {
    const auto first = model.firstRetainedRow();
    const auto last = first + model.historySize() + model.rows() - 1;
    return model.copyText({first, 0}, {last, model.columns() - 1}, 1 << 20)
        .value_or("<none>");
}
std::size_t count(const std::string &text, const std::string &needle) {
    std::size_t result = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        ++result;
    }
    return result;
}
// What an Ink-style program does after SIGWINCH: go up over its previous
// frame, erase to the end of the screen and print the frame again.
const char kFrame[] = "alpha\r\nbeta\r\ngamma\r\n";
const char kRedraw[] = "\x1b[3A\x1b[Jalpha\r\nbeta\r\ngamma\r\n";
} // namespace

TEST_SUIT_BEGIN(TerminalOperations)

TEST_CASE("Empty allocated screens never offer history scrolling") {
    for (bool alternate : {false, true}) {
        TerminalModel model(8, 4, 20);
        VtParser parser(model);
        for (unsigned stage = 0; stage < 5; ++stage) {
            model.setAlternateScreen(alternate);
            if (stage == 1)
                parser.feed("text\x1b[2J");
            if (stage == 2)
                model.reset();
            if (stage == 3)
                model.resize(10, 7);
            if (stage == 4)
                model.resize(5, 2);
            model.setAlternateScreen(alternate);
            model.scrollBy(1000);
            model.scrollToOldest();
            model.advanceViewport(10);
            EXPECT_EQ(model.historySize(), std::size_t(0));
            EXPECT_EQ(model.viewport().offset(), 0.0);
            EXPECT_EQ(model.viewport().targetOffset(), 0.0);
            EXPECT_FALSE(model.frame(16).scrollbar.visible);
            EXPECT_EQ(model.frame(16).verticalOffsetPixels, 0.0f);
        }
    }
}

TEST_CASE(
    "Intentional empty output rows remain history after display erasure") {
    TerminalModel model(8, 3, 20);
    VtParser parser(model);
    parser.feed("\r\n\r\n\r\n\r\n\r\n");
    EXPECT_EQ(model.historySize(), std::size_t(3));
    parser.feed("\x1b[2J"); // ED2 clears the display, not saved lines.
    model.scrollToOldest();
    model.advanceViewport(10);
    EXPECT_EQ(model.historySize(), std::size_t(3));
    EXPECT_EQ(model.viewport().offset(), 3.0);
    EXPECT(model.frame(16).scrollbar.visible);
}

TEST_CASE(
    "Shrinking the height keeps a short TUI frame out of the scrollback") {
    TerminalModel model(20, 10, 100);
    VtParser parser(model);
    parser.feed(kFrame);
    model.resize(20, 5); // Rows below the cursor are blank: drop those first.
    EXPECT_EQ(model.historySize(), std::size_t(0));
    EXPECT_EQ(model.cursor().row, 3);
    EXPECT(model.cell(0, 0).codePoint == U'a');
    parser.feed(kRedraw);
    const auto text = everything(model);
    EXPECT_EQ(count(text, "alpha"), std::size_t(1));
    EXPECT_EQ(count(text, "beta"), std::size_t(1));
    EXPECT_EQ(count(text, "gamma"), std::size_t(1));
}

TEST_CASE("Never-written cells use the default colors like cleared cells") {
    TerminalModel model(20, 10, 100);
    const auto fresh = model.cell(5, 5);
    EXPECT(fresh.backgroundColor().kind == ColorReference::Kind::Default);
    EXPECT(fresh.foregroundColor().kind == ColorReference::Kind::Default);
    model.write(U"x");
    model.eraseDisplay(TerminalModel::EraseMode::All);
    const auto cleared = model.cell(5, 5);
    EXPECT(cleared.backgroundColor().kind == fresh.backgroundColor().kind);
}
TEST_CASE("Resizing back and forth under a TUI never duplicates its rows") {
    TerminalModel model(20, 10, 100);
    VtParser parser(model);
    parser.feed("old output\r\n");
    parser.feed(kFrame);
    for (const auto rows : {6, 4, 12, 5, 10}) {
        model.resize(20, std::size_t(rows));
        parser.feed(kRedraw);
        const auto text = everything(model);
        EXPECT_EQ(count(text, "alpha"), std::size_t(1));
        EXPECT_EQ(count(text, "gamma"), std::size_t(1));
        EXPECT_EQ(count(text, "old output"), std::size_t(1));
    }
}

TEST_CASE(
    "Shrinking keeps content below the cursor and drops only blank rows") {
    TerminalModel model(20, 8, 100);
    VtParser parser(model);
    parser.feed("top\r\n\x1b[8;1Hstatus\x1b[1;1H");
    model.resize(20, 4); // The status row is not blank: nothing to drop.
    EXPECT(model.cell(0, 3).codePoint == U's');
    EXPECT_EQ(model.historySize(), std::size_t(4));
    TerminalModel colored(20, 8, 100);
    VtParser coloredParser(colored);
    coloredParser.feed("\x1b[1;1Htop\x1b[7;1H\x1b[44m\x1b[K\x1b[0m\x1b[1;1H");
    colored.resize(20, 4); // A colored row is content: it is kept, not dropped.
    EXPECT(colored.cell(0, 3).backgroundColor().kind ==
           ColorReference::Kind::Indexed);
}

TEST_CASE("A resize that must scroll still puts the cursor on its content") {
    TerminalModel model(20, 6, 100);
    VtParser parser(model);
    parser.feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
    model.resize(20, 3);
    EXPECT_EQ(model.cursor().row, 2);
    EXPECT_EQ(model.historySize(), std::size_t(3));
    EXPECT(model.cell(0, 2).codePoint == U'6');
}

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
    const Palette palette;
    EXPECT_EQ(palette.resolve(model.cell(0, 0)).foreground.r, 4);
    EXPECT_EQ(palette.resolve(model.cell(0, 0)).background.r, 1);
    EXPECT_EQ(palette.resolve(model.cell(1, 0)).foreground.r, 1);
    EXPECT_EQ(palette.resolve(model.cell(2, 0)).foreground.r, 4);
    EXPECT_EQ(palette.resolve(model.cell(2, 0)).background.r, 4);
    EXPECT_EQ(palette.resolve(model.cell(3, 0)).foreground.r, 1);
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

TEST_CASE("SGR text attributes are compact saved and restored independently") {
    TerminalModel model(10, 2);
    VtParser parser(model);
    parser.feed("\x1b[1;2;3;4;5;9mA\x1b[22;23;24;25;29mB\x1b[1;4mC\x1b"
                "7\x1b[0m!\x1b"
                "8D");
    const auto flags = model.cell(0, 0).attributes;
    EXPECT(hasAttribute(flags, CellAttribute::Bold));
    EXPECT(hasAttribute(flags, CellAttribute::Faint));
    EXPECT(hasAttribute(flags, CellAttribute::Italic));
    EXPECT(hasAttribute(flags, CellAttribute::Underline));
    EXPECT(hasAttribute(flags, CellAttribute::Strike));
    EXPECT(hasAttribute(flags, CellAttribute::Blink));
    EXPECT(model.cell(1, 0).style() == CellAttribute::None);
    EXPECT(model.cell(3, 0).codePoint == U'D');
    EXPECT(hasAttribute(model.cell(3, 0).attributes, CellAttribute::Bold));
    EXPECT(hasAttribute(model.cell(3, 0).attributes, CellAttribute::Underline));
    parser.feed("\x1b[?1049hX\x1b[0m\x1b[?1049lY");
    EXPECT(hasAttribute(model.cell(4, 0).attributes, CellAttribute::Bold));
    model.moveCursor(0, 1);
    model.eraseLine(TerminalModel::EraseMode::All);
    EXPECT(model.cell(0, 1).style() == CellAttribute::None);
    EXPECT_EQ(sizeof(FChar), std::size_t(12));
}

TEST_CASE("HTS TBC forward and backward tabs use terminal-wide custom stops") {
    TerminalModel model(20, 2);
    VtParser parser(model);
    parser.feed("\x1b[4G\x1bH\x1b[H\t");
    EXPECT_EQ(model.cursor().column, 3);
    parser.feed("\x1b[g\x1b[H\t");
    EXPECT_EQ(model.cursor().column, 8);
    parser.feed("\x1b[3g\x1b[H\t");
    EXPECT_EQ(model.cursor().column, 19);
    parser.feed("\x1b[Z");
    EXPECT_EQ(model.cursor().column, 0);
    parser.feed("\x1b[6G\x1bH\x1b[14G\x1bH\x1b[H\x1b[2I");
    EXPECT_EQ(model.cursor().column, 13);
    parser.feed("\x1b[Z");
    EXPECT_EQ(model.cursor().column, 5);
    parser.feed("\x1b[?1049h");
    model.tabForward();
    EXPECT_EQ(model.cursor().column, 5);
}

TEST_CASE("Cursor shape steady and blink controls do not cancel pending wrap") {
    TerminalModel model(3, 2);
    VtParser parser(model);
    parser.feed("abc\x1b[6 q\x1b[?25lD");
    EXPECT(model.cursor().shape == CursorState::Shape::Bar);
    EXPECT_FALSE(model.cursor().blink);
    EXPECT_FALSE(model.cursor().visible);
    EXPECT(model.cell(0, 1).codePoint == U'D');
    parser.feed("\x1b[?12h\x1b[?25h\x1b[4 q");
    EXPECT(model.cursor().shape == CursorState::Shape::Underline);
    EXPECT_FALSE(model.cursor().blink);
    parser.feed("\x1b[1 q");
    EXPECT(model.cursor().shape == CursorState::Shape::Block);
    EXPECT(model.cursor().blink);
}

TEST_CASE("REP repeats the last printed character") {
    TerminalModel model(8, 2);
    VtParser parser(model);
    parser.feed("\x1b[3bx\x1b[3b");
    EXPECT(model.cell(0, 0).codePoint == U'x');
    EXPECT(model.cell(3, 0).codePoint == U'x');
    EXPECT(model.cell(4, 0).codePoint == U' ');
}

TEST_CASE("Soft reset restores modes and rendition but keeps the screen") {
    TerminalModel model(8, 4);
    VtParser parser(model);
    parser.feed("\x1b[1;31mabc\x1b[?1h\x1b[?1000h\x1b[?1006h\x1b[?25l"
                "\x1b[2;3r\x1b[?6h\x1b[4h\x1b[!p");
    EXPECT_FALSE(parser.modes().applicationCursor);
    EXPECT(parser.modes().mouseTracking == TerminalModes::MouseTracking::Off);
    EXPECT(parser.modes().mouseEncoding == TerminalModes::MouseEncoding::X10);
    EXPECT(model.cursor().visible);
    EXPECT_FALSE(model.originMode());
    EXPECT_EQ(model.scrollTop(), 0);
    EXPECT_EQ(model.scrollBottom(), 3);
    EXPECT(model.cell(1, 0).codePoint == U'b');
    parser.feed("Z"); // Origin reset homes the cursor, over the old 'a'.
    EXPECT(model.cell(0, 0).codePoint == U'Z');
    EXPECT_EQ(int(model.cell(0, 0).foregroundColor().kind),
              int(ColorReference::Kind::Default));
}

TEST_CASE("Alternate screen modes 47 and 1047 and cursor save mode 1048") {
    TerminalModel model(8, 2);
    VtParser parser(model);
    parser.feed("\x1b[?47h");
    EXPECT(model.alternateScreen());
    parser.feed("\x1b[?47l\x1b[?1047h");
    EXPECT(model.alternateScreen());
    parser.feed("\x1b[?1047l\x1b[3C\x1b[?1048h\x1b[H\x1b[?1048l");
    EXPECT_FALSE(model.alternateScreen());
    EXPECT_EQ(model.cursor().column, 3);
}

TEST_CASE("OSC 4 and OSC 104 change and reset client palette entries") {
    TerminalModel model(8, 2);
    std::vector<std::pair<std::size_t, Rgb8>> set;
    std::vector<std::optional<std::size_t>> reset;
    std::string reply;
    Palette palette;
    palette.colors[3] = {1, 2, 3};
    VtEvents events;
    events.reply = [&](std::string_view bytes) { reply = bytes; };
    events.setPaletteColor = [&](std::size_t index, Rgb8 color) {
        set.emplace_back(index, color);
    };
    events.resetPaletteColor = [&](std::optional<std::size_t> index) {
        reset.push_back(index);
    };
    events.palette = &palette;
    VtParser parser(model, events);
    parser.feed("\x1b]4;1;rgb:ff/00/80;2;#0a0b0c\x1b\\");
    EXPECT_EQ(set.size(), std::size_t(2));
    EXPECT_EQ(set[0].first, std::size_t(1));
    EXPECT_EQ(int(set[0].second.r), 255);
    EXPECT_EQ(int(set[0].second.g), 0);
    EXPECT_EQ(int(set[0].second.b), 128);
    EXPECT_EQ(int(set[1].second.g), 11);
    parser.feed("\x1b]4;300;rgb:ff/ff/ff\x07\x1b]4;5;nonsense\x07");
    EXPECT_EQ(set.size(), std::size_t(2));
    parser.feed("\x1b]4;3;?\x07");
    EXPECT_EQ(reply, std::string("\x1b]4;3;rgb:0101/0202/0303\x1b\\"));
    parser.feed("\x1b]104\x07\x1b]104;7;9\x07");
    EXPECT_EQ(reset.size(), std::size_t(3));
    EXPECT_FALSE(reset[0].has_value());
    EXPECT_EQ(*reset[1], std::size_t(7));
    EXPECT_EQ(*reset[2], std::size_t(9));
}

TEST_SUIT_END
