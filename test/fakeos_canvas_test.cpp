#include "canvas.h"
#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

using namespace fakeos;

namespace {
std::string rowText(const TerminalModel &model, std::size_t y) {
    std::string text;
    for (std::size_t x = 0; x < model.columns(); ++x) {
        const auto cp = model.cell(x, y).codePoint;
        text.push_back(cp >= 32 && cp < 127 ? char(cp) : ' ');
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}
} // namespace

TEST_SUIT_BEGIN(FakeosCanvas)

TEST_CASE("An unchanged canvas flushes to nothing") {
    AsciiCanvas canvas(10, 3);
    canvas.text(0, 0, "hi", {255, 255, 255});
    EXPECT(!canvas.flush().empty());
    EXPECT(canvas.flush().empty());
    canvas.text(0, 0, "hi", {255, 255, 255}); // Same content again.
    EXPECT(canvas.flush().empty());
}

TEST_CASE("Only changed cells are emitted and the screen ends up right") {
    TerminalModel model(10, 3);
    VtParser parser(model);
    AsciiCanvas canvas(10, 3);
    canvas.text(0, 0, "hello", {255, 255, 255});
    canvas.text(0, 2, "bottom", {255, 255, 255});
    parser.feed(canvas.flush());
    EXPECT(rowText(model, 0) == "hello");
    EXPECT(rowText(model, 2) == "bottom");
    canvas.put(1, 0, U'a', {255, 0, 0}, {0, 0, 0});
    const auto update = canvas.flush();
    EXPECT(update.size() < 40); // One cell, not the whole screen.
    EXPECT(update.find("bottom") == std::string::npos);
    parser.feed(update);
    EXPECT(rowText(model, 0) == "hallo");
    EXPECT(model.cell(1, 0).foreground.r == 255);
}

TEST_CASE("Drawing outside the canvas is ignored; text is clipped") {
    AsciiCanvas canvas(4, 2);
    canvas.put(-1, 0, U'x', {1, 1, 1}, {0, 0, 0});
    canvas.put(4, 0, U'x', {1, 1, 1}, {0, 0, 0});
    canvas.text(2, 1, "abcdef", {1, 1, 1});
    EXPECT(canvas.at(2, 1).codePoint == U'a');
    EXPECT(canvas.at(3, 1).codePoint == U'b');
}

TEST_CASE("Resize and invalidate redraw everything") {
    AsciiCanvas canvas(5, 1);
    canvas.text(0, 0, "abcde", {9, 9, 9});
    canvas.flush();
    canvas.invalidate();
    EXPECT(canvas.flush().find("abcde") != std::string::npos);
    canvas.resize(6, 2);
    EXPECT(canvas.columns() == 6 && canvas.rows() == 2);
    EXPECT(!canvas.flush().empty());
}

TEST_CASE("Colors follow the terminal's capabilities") {
    Capabilities truecolor, indexed, basic;
    indexed.truecolor = false;
    basic.truecolor = basic.colors256 = false;
    for (const auto &caps : {truecolor, indexed, basic}) {
        AsciiCanvas canvas(2, 1, caps);
        canvas.put(0, 0, U'x', {255, 128, 0}, {0, 0, 64});
        const auto out = canvas.flush();
        EXPECT((out.find("38;2;255;128;0") != std::string::npos) ==
               caps.truecolor);
        EXPECT((out.find("38;5;") != std::string::npos) ==
               (!caps.truecolor && caps.colors256));
    }
    EXPECT_EQ(nearestXterm256({255, 0, 0}), 196);
    EXPECT_EQ(nearestXterm256({0, 0, 0}), 16);
    EXPECT_EQ(nearestAnsi16({255, 0, 0}), 9);
    EXPECT_EQ(nearestAnsi16({0, 0, 0}), 0);
}

TEST_CASE("wordAt returns the text drawn under a cell") {
    AsciiCanvas canvas(30, 3);
    canvas.text(2, 1, "( blue )  [red]  plain.", {1, 1, 1});
    EXPECT(canvas.wordAt(5, 1) == "blue");
    EXPECT(canvas.wordAt(3, 1) == ""); // The parenthesis is not part of a word.
    EXPECT(canvas.wordAt(13, 1) == "red");
    EXPECT(canvas.wordAt(20, 1) == "plain.");
    EXPECT(canvas.wordAt(0, 1) == "");
    EXPECT(canvas.wordAt(40, 1) == "");
    EXPECT(canvas.wordAt(5, -1) == "");
}

TEST_CASE("UTF-8 helpers round trip") {
    const std::string text = "a\xc3\xa5\xe2\x82\xac\xf0\x9f\x94\xa5";
    const auto decoded = decodeUtf8(text);
    EXPECT_EQ(decoded.size(), std::size_t(4));
    std::string back;
    for (const auto cp : decoded) {
        appendUtf8(back, cp);
    }
    EXPECT(back == text);
}

TEST_SUIT_END
