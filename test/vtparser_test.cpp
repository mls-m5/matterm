#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {
struct Snapshot {
    std::u32string text;
    std::vector<unsigned> colors;
    int column;
    int row;
    bool visible;
    bool blink;
    CursorState::Shape shape;
    bool alternate;
    bool applicationCursor;
    bool bracketedPaste;
    bool origin;
    int top;
    int bottom;
    std::size_t history;
    std::vector<std::string> replies;
    std::vector<std::string> titles;

    bool operator==(const Snapshot &other) const {
        return text == other.text && colors == other.colors &&
               column == other.column && row == other.row &&
               visible == other.visible && blink == other.blink &&
               shape == other.shape && alternate == other.alternate &&
               history == other.history && replies == other.replies &&
               titles == other.titles &&
               applicationCursor == other.applicationCursor &&
               bracketedPaste == other.bracketedPaste &&
               origin == other.origin && top == other.top &&
               bottom == other.bottom;
    }
};

Snapshot parse(std::string_view bytes,
               std::size_t split,
               bool oneByte = false) {
    TerminalModel model(12, 5, 8);
    std::vector<std::string> replies, titles;
    VtParser parser(
        model,
        {[&](std::string_view value) { replies.emplace_back(value); },
         [&](std::string_view value) { titles.emplace_back(value); }});
    if (oneByte) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            parser.feed(bytes.substr(i, 1));
        }
    }
    else {
        parser.feed(bytes.substr(0, split));
        parser.feed(bytes.substr(split));
    }
    parser.finish();
    Snapshot result;
    // Include historical rows as well as the live screen in the comparison.
    model.scrollToOldest();
    model.advanceViewport(10);
    const auto cursor = model.cursor();
    result.column = cursor.column;
    result.row = cursor.row;
    result.visible = cursor.visible;
    result.blink = cursor.blink;
    result.shape = cursor.shape;
    result.alternate = model.alternateScreen();
    result.applicationCursor = parser.modes().applicationCursor;
    result.bracketedPaste = parser.modes().bracketedPaste;
    result.origin = model.originMode();
    result.top = model.scrollTop();
    result.bottom = model.scrollBottom();
    result.history = model.historySize();
    // Screen cells plus oldest visible page catch both scroll and update
    // errors.
    for (std::size_t y = 0; y < model.rows(); ++y) {
        for (std::size_t x = 0; x < model.columns(); ++x) {
            const auto &cell = model.cell(x, y);
            result.text.push_back(cell.codePoint);
            result.colors.push_back(unsigned(cell.attributes));
            result.colors.push_back(unsigned(cell.foreground.r) << 16 |
                                    unsigned(cell.foreground.g) << 8 |
                                    cell.foreground.b);
            result.colors.push_back(unsigned(cell.background.r) << 16 |
                                    unsigned(cell.background.g) << 8 |
                                    cell.background.b);
        }
    }
    const auto frame = model.frame(20);
    for (std::size_t y = 0; y < frame.cells.rows(); ++y) {
        for (std::size_t x = 0; x < frame.cells.columns(); ++x) {
            result.text.push_back(frame.cells.row(y).data()[x].codePoint);
        }
    }
    result.replies = std::move(replies);
    result.titles = std::move(titles);
    return result;
}
} // namespace

TEST_SUIT_BEGIN(VtParser)

TEST_CASE("Text controls cursor movement and omitted or zero parameters") {
    TerminalModel model(10, 4);
    VtParser parser(model);
    parser.feed("aåäö\r\nXYZ\b!\tQ");
    EXPECT(model.cell(0, 0).codePoint == U'a');
    EXPECT(model.cell(3, 0).codePoint == U'ö');
    EXPECT(model.cell(2, 1).codePoint == U'!');
    EXPECT(model.cell(8, 1).codePoint == U'Q');
    parser.feed("\x1b[0;0H\x1b[2B\x1b[3CX");
    EXPECT(model.cell(3, 2).codePoint == U'X');
    parser.feed("\x1b[A\x1b[D\x1b[2EY\x1b[1FZ");
    EXPECT(model.cell(0, 3).codePoint == U'Y');
    EXPECT(model.cell(0, 2).codePoint == U'Z');
    parser.feed("\x1b[999999;999999H");
    EXPECT_EQ(model.cursor().column, 9);
    EXPECT_EQ(model.cursor().row, 3);
}

TEST_CASE("Erasure uses current colors and does not remove history") {
    TerminalModel model(4, 2, 4);
    VtParser parser(model);
    parser.feed("aaaa\r\nbbbb\r\ncccc\x1b[48;2;1;2;3m\x1b[1;2H\x1b[K");
    EXPECT(model.cell(0, 0).codePoint == U'b');
    EXPECT(model.cell(1, 0).codePoint == U' ');
    EXPECT_EQ(model.cell(1, 0).background.b, 3);
    EXPECT_EQ(model.historySize(), std::size_t(1));
    parser.feed("\x1b[2J");
    EXPECT(model.cell(0, 1).codePoint == U' ');
    EXPECT_EQ(model.historySize(), std::size_t(1));
    EXPECT_EQ(model.cursor().column, 1);
}

TEST_CASE("SGR supports basic bright indexed RGB and configurable defaults") {
    TerminalModel model(12, 2);
    VtParser parser(model, {}, {10, 20, 30}, {40, 50, 60});
    parser.feed("\x1b[31;44mA\x1b[92;104mB\x1b[38;5;196;48;5;232mC"
                "\x1b[38;2;1;2;3;48;2;4;5;6mD\x1b[39;49mE\x1b[31mF\x1b[mG");
    EXPECT_EQ(model.cell(0, 0).foreground.r, 205);
    EXPECT_EQ(model.cell(0, 0).background.b, 238);
    EXPECT_EQ(model.cell(1, 0).foreground.g, 255);
    EXPECT_EQ(model.cell(1, 0).background.r, 92);
    EXPECT_EQ(model.cell(2, 0).foreground.r, 255);
    EXPECT_EQ(model.cell(2, 0).background.r, 8);
    EXPECT_EQ(model.cell(3, 0).foreground.b, 3);
    EXPECT_EQ(model.cell(3, 0).background.b, 6);
    EXPECT_EQ(model.cell(4, 0).foreground.g, 20);
    EXPECT_EQ(model.cell(4, 0).background.g, 50);
    EXPECT_EQ(model.cell(6, 0).foreground.r, 10);
    parser.feed("\x1b[38;5;256mH\x1b[38;2;999;0;0mI\x1b[38:2:1:2:3mJ");
    EXPECT_EQ(model.cell(7, 0).foreground.r, 10);
    EXPECT_EQ(model.cell(8, 0).foreground.r, 10);
    EXPECT_EQ(model.cell(9, 0).foreground.r, 10);
}

TEST_CASE(
    "Saved cursor rendition and pending wrap survive visibility changes") {
    TerminalModel model(3, 3);
    VtParser parser(model);
    parser.feed("\x1b[31mabc\x1b"
                "7\x1b[?25l\x1b[H\x1b[32mX\x1b"
                "8D");
    EXPECT(model.cell(0, 1).codePoint == U'D');
    EXPECT_EQ(model.cell(0, 1).foreground.r, 205);
    EXPECT(model.cursor().visible);
    parser.feed("\x1b[s\x1b[3;3H\x1b[uE");
    EXPECT(model.cell(1, 1).codePoint == U'E');
    parser.feed("\x1b[?25l");
    EXPECT_FALSE(model.cursor().visible);
    parser.feed("\x1b[?25h");
    EXPECT(model.cursor().visible);
}

TEST_CASE(
    "1049 alternate screen restores primary text style cursor and viewport") {
    TerminalModel model(8, 2, 4);
    VtParser parser(model);
    parser.feed("one\r\ntwo\r\n\x1b[31mthree");
    model.scrollBy(0.5);
    model.advanceViewport(10);
    const auto cursor = model.cursor();
    const auto offset = model.viewport().offset();
    parser.feed("\x1b[?1049h\x1b[32mALT\x1b[?1049h");
    EXPECT(model.alternateScreen());
    EXPECT(model.cell(0, 0).codePoint == U'A');
    EXPECT_EQ(model.historySize(), std::size_t(0));
    parser.feed("\r\n1\r\n2\r\n3\x1b[?1049lZ");
    EXPECT_FALSE(model.alternateScreen());
    EXPECT_EQ(model.cursor().row, cursor.row);
    EXPECT_EQ(model.viewport().offset(), offset);
    EXPECT(model.cell(std::size_t(cursor.column), std::size_t(cursor.row))
               .codePoint == U'Z');
    EXPECT_EQ(model.cell(std::size_t(cursor.column), std::size_t(cursor.row))
                  .foreground.r,
              205);
}

TEST_CASE("OSC titles and DSR replies are explicit events not rendered text") {
    TerminalModel model(10, 4);
    std::vector<std::string> replies, titles;
    VtParser parser(
        model,
        {[&](std::string_view value) { replies.emplace_back(value); },
         [&](std::string_view value) { titles.emplace_back(value); }});
    parser.feed("\x1b]0;hello\a\x1b]2;world\x1b\\\x1b]52;c;secret\a"
                "\x1b[2;3H\x1b[5n\x1b[6n\x1b[?6nX");
    EXPECT_EQ(titles.size(), std::size_t(2));
    EXPECT_EQ(titles[0], std::string("hello"));
    EXPECT_EQ(titles[1], std::string("world"));
    EXPECT_EQ(replies.size(), std::size_t(2));
    EXPECT_EQ(replies[0], std::string("\x1b[0n"));
    EXPECT_EQ(replies[1], std::string("\x1b[2;3R"));
    EXPECT(model.cell(2, 1).codePoint == U'X');
    EXPECT(model.cell(0, 0).codePoint == U' ');
}

TEST_CASE("Unsupported controls strings and character-set escapes do not leak "
          "payload") {
    TerminalModel model(20, 2);
    VtParser parser(model);
    parser.feed("A\x1bPsecret\aother\x1b\\B\x1b_hidden\x1b\\C\x1b(BD"
                "\x1b[?9999hE\x1b[2 qF");
    const std::u32string expected = U"ABCDEF";
    for (std::size_t x = 0; x < expected.size(); ++x) {
        EXPECT(model.cell(x, 0).codePoint == expected[x]);
    }
}

TEST_CASE(
    "Oversized sequences are bounded discarded and recover at terminators") {
    TerminalModel model(10, 2);
    int titles = 0;
    VtParser parser(model, {{}, [&](std::string_view) { ++titles; }});
    parser.feed("\x1b[" + std::string(10000, '9') + "mA");
    parser.feed("\x1b[" + std::string(100, ';') + "HB");
    parser.feed("\x1b]2;" + std::string(10000, 'x') + "\aC");
    parser.feed("\x1b[31\x18"
                "D\x1b]2;cancel\x1a"
                "E");
    EXPECT_EQ(titles, 0);
    const std::u32string expected = U"ABCDE";
    for (std::size_t x = 0; x < expected.size(); ++x) {
        EXPECT(model.cell(x, 0).codePoint == expected[x]);
        EXPECT_EQ(model.cell(x, 0).foreground.g, 255);
    }
}

TEST_CASE(
    "End of stream flushes UTF-8 but never paints incomplete control payload") {
    TerminalModel model(10, 2);
    VtParser parser(model);
    parser.feed("A\xc3");
    parser.finish();
    EXPECT(model.cell(1, 0).codePoint == U'\ufffd');
    parser.feed("\x1b[31");
    parser.finish();
    parser.feed("B\x1b]2;unfinished");
    parser.finish();
    parser.feed("C");
    EXPECT(model.cell(2, 0).codePoint == U'B');
    EXPECT(model.cell(3, 0).codePoint == U'C');
    parser.feed("\xe2");
    parser.resetInput();
    parser.feed("D");
    EXPECT(model.cell(4, 0).codePoint == U'D');
}

TEST_CASE("Whole input every split and one-byte chunks yield identical state "
          "and events") {
    const std::vector<std::string> cases{
        "hello åäö\r\nworld\x1b[2;3H\x1b[38;2;12;34;56mX\x1b[6n",
        "\x1b]0;title\a\x1b]2;other\x1b\\\x1bPdiscard\x1b\\Q",
        "\x1b[31mhello\x1b"
        "7\x1b[3;3H\x1b[32mX\x1b"
        "8Z\x1b[?25l",
        "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\x1b[?1049hALT\x1b[?1049l",
        "\xe2\x82\x1b[999999999999999mX\x1b[31\x18"
        "A\x1b]2;cancel\x1a"
        "B",
        "\x1b[38;5;196;48;5;240mred\x1b[0mreset\x1b[2K",
        "\x1b[2;4r\x1b[?6h\x1b[2;2H\x1b[L\x1b[M\x1b[?1h\x1b[?2004h",
        "\x1b(0lqqk\x1b(B\x1b[7mreverse\x1b[27m\x1b[2@X\x1b[P\x1b[X",
        "partial\xc3",
        "partial\x1b[38;2;",
        "partial\x1b]2;title\x1b"};
    for (const auto &bytes : cases) {
        const auto expected = parse(bytes, bytes.size());
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            EXPECT(parse(bytes, split) == expected);
        }
        EXPECT(parse(bytes, 0, true) == expected);
    }
}

TEST_CASE("Deterministic arbitrary bytes are independent of chunk boundaries") {
    std::uint32_t random = 0x12345678;
    for (int trial = 0; trial < 40; ++trial) {
        std::string bytes;
        for (int i = 0; i < 256; ++i) {
            random = random * 1664525u + 1013904223u;
            bytes.push_back(char(random >> 24));
        }
        const auto expected = parse(bytes, bytes.size());
        EXPECT(parse(bytes, 0, true) == expected);
        EXPECT(parse(bytes, 1) == expected);
        EXPECT(parse(bytes, 127) == expected);
    }
}

TEST_CASE("OSC length boundary is exact and excessive parameters never apply "
          "partial SGR") {
    TerminalModel model(10, 2);
    std::vector<std::string> titles;
    VtParser parser(model, {{}, [&](std::string_view title) {
                                titles.emplace_back(title);
                            }});
    parser.feed("\x1b]2;" + std::string(2046, 'x') + "\a");
    parser.feed("\x1b]2;" + std::string(2047, 'x') + "\a");
    EXPECT_EQ(titles.size(), std::size_t(1));
    EXPECT_EQ(titles[0].size(), std::size_t(2046));
    std::string sgr = "\x1b[";
    for (int i = 0; i < 31; ++i) {
        sgr += "0;";
    }
    parser.feed(sgr + "31mA");
    EXPECT_EQ(model.cell(0, 0).foreground.r, 205);
    parser.feed(sgr + "0;32mB"); // 33 parameters: ignore the entire sequence.
    EXPECT_EQ(model.cell(1, 0).foreground.r, 205);
    EXPECT_EQ(model.cell(1, 0).foreground.g, 0);
}

TEST_SUIT_END
