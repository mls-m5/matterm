#include "colorreference.h"
#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "tty/canvas.h"
#include "tty/utf8.h"
#include "vtparser.h"

using namespace tty;

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

TEST_SUIT_BEGIN(TtyCanvas)

TEST_CASE("An unchanged canvas flushes to nothing") {
    Canvas canvas(10, 3);
    canvas.text(0, 0, "hi", {255, 255, 255});
    EXPECT(!canvas.flush().empty());
    EXPECT(canvas.flush().empty());
    canvas.text(0, 0, "hi", {255, 255, 255}); // Same content again.
    EXPECT(canvas.flush().empty());
}

TEST_CASE("Only changed cells are emitted and the screen ends up right") {
    TerminalModel model(10, 3);
    VtParser parser(model);
    Canvas canvas(10, 3);
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
    Canvas canvas(4, 2);
    canvas.put(-1, 0, U'x', {1, 1, 1}, {0, 0, 0});
    canvas.put(4, 0, U'x', {1, 1, 1}, {0, 0, 0});
    canvas.text(2, 1, "abcdef", {1, 1, 1});
    EXPECT(canvas.at(2, 1).codePoint == U'a');
    EXPECT(canvas.at(3, 1).codePoint == U'b');
}

TEST_CASE("Resize and invalidate redraw everything") {
    Canvas canvas(5, 1);
    canvas.text(0, 0, "abcde", {9, 9, 9});
    canvas.flush();
    canvas.invalidate();
    EXPECT(canvas.flush().find("abcde") != std::string::npos);
    canvas.resize(6, 2);
    EXPECT(canvas.columns() == 6 && canvas.rows() == 2);
    EXPECT(!canvas.flush().empty());
}

TEST_CASE("Colors follow the terminal's capabilities") {
    Caps truecolor, indexed, basic;
    indexed.truecolor = false;
    basic.truecolor = basic.colors256 = false;
    for (const auto &caps : {truecolor, indexed, basic}) {
        Canvas canvas(2, 1, caps);
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
    Canvas canvas(30, 3);
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

TEST_CASE("Attributes reach the terminal") {
    TerminalModel model(4, 1);
    VtParser parser(model);
    Canvas canvas(4, 1);
    Cell cell;
    cell.codePoint = U'a';
    cell.italic = cell.underline = cell.strike = cell.faint = true;
    canvas.put(0, 0, cell);
    Cell other;
    other.codePoint = U'b';
    other.inverse = other.blink = other.conceal = other.bold = true;
    canvas.put(1, 0, other);
    canvas.put(2, 0, U'c', {200, 200, 200}, {0, 0, 0});
    parser.feed(canvas.flush());
    const auto a = model.cell(0, 0).attributes;
    EXPECT(hasAttribute(a, CellAttribute::Italic));
    EXPECT(hasAttribute(a, CellAttribute::Underline));
    EXPECT(hasAttribute(a, CellAttribute::Strike));
    EXPECT(hasAttribute(a, CellAttribute::Faint));
    EXPECT(!hasAttribute(a, CellAttribute::Bold));
    const auto b = model.cell(1, 0).attributes;
    EXPECT(hasAttribute(b, CellAttribute::Inverse));
    EXPECT(hasAttribute(b, CellAttribute::Blink));
    EXPECT(hasAttribute(b, CellAttribute::Conceal));
    EXPECT(hasAttribute(b, CellAttribute::Bold));
    EXPECT(!hasAttribute(b, CellAttribute::Italic));
    EXPECT(model.cell(2, 0).style() == CellAttribute::None);
}

TEST_CASE("Default colours and palette indices pass through") {
    TerminalModel model(3, 1);
    VtParser parser(model);
    Canvas canvas(3, 1);
    Cell plain;
    plain.codePoint = U'a';
    plain.defaultForeground = plain.defaultBackground = true;
    canvas.put(0, 0, plain);
    Cell red;
    red.codePoint = U'b';
    red.foregroundIndex = 1;
    red.backgroundIndex = 12;
    canvas.put(1, 0, red);
    Cell far;
    far.codePoint = U'c';
    far.foregroundIndex = 200;
    canvas.put(2, 0, far);
    const auto out = canvas.flush();
    EXPECT(out.find("38;2;200;200;200") == std::string::npos);
    EXPECT(out.find(";31;104m") != std::string::npos);
    EXPECT(out.find("38;5;200;") != std::string::npos);
    parser.feed(out);
    EXPECT(model.cell(0, 0).foregroundColor().kind ==
           ColorReference::Kind::Default);
    EXPECT(model.cell(0, 0).backgroundColor().kind ==
           ColorReference::Kind::Default);
    EXPECT(model.cell(1, 0).foregroundColor().kind ==
           ColorReference::Kind::Indexed);
    EXPECT_EQ(int(model.cell(1, 0).foregroundColor().value.r), 1);
    EXPECT_EQ(int(model.cell(1, 0).backgroundColor().value.r), 12);
    EXPECT_EQ(int(model.cell(2, 0).foregroundColor().value.r), 200);
}

TEST_CASE("A palette index above 15 is reduced where there is no 256 mode") {
    Caps basic;
    basic.truecolor = basic.colors256 = false;
    Canvas canvas(1, 1, basic);
    Cell cell;
    cell.codePoint = U'x';
    cell.foregroundIndex = 200;
    cell.foreground = {255, 0, 0};
    canvas.put(0, 0, cell);
    const auto out = canvas.flush();
    EXPECT(out.find("38;5;") == std::string::npos);
    EXPECT(out.find(";91;") != std::string::npos); // From the RGB fallback.
}

TEST_CASE("Wide characters take two columns and hide the cell after them") {
    Canvas canvas(6, 1);
    canvas.setWideChars(true);
    canvas.put(0, 0, U'\u4e16', {1, 1, 1}, {0, 0, 0});
    canvas.put(1, 0, U'x', {1, 1, 1}, {0, 0, 0}); // Covered by the wide one.
    canvas.put(2, 0, U'y', {1, 1, 1}, {0, 0, 0});
    auto out = canvas.flush();
    EXPECT(out.find("\xe4\xb8\x96y") != std::string::npos); // No move between.
    EXPECT(out.find('x') == std::string::npos);
    // Changing only the covered cell shows nothing; changing the wide one does.
    canvas.put(1, 0, U'z', {1, 1, 1}, {0, 0, 0});
    EXPECT(canvas.flush().empty());
    canvas.put(0, 0, U'\u3042', {1, 1, 1}, {0, 0, 0});
    EXPECT(!canvas.flush().empty());
}

TEST_CASE("Replacing a wide character by a narrow one redraws the cell after") {
    Canvas canvas(4, 1);
    canvas.setWideChars(true);
    canvas.put(0, 0, U'\u4e16', {1, 1, 1}, {0, 0, 0});
    canvas.put(1, 0, U'z', {1, 1, 1}, {0, 0, 0});
    canvas.flush();
    canvas.put(0, 0, U'a', {1, 1, 1}, {0, 0, 0});
    const auto out = canvas.flush();
    EXPECT(out.find('a') != std::string::npos);
    EXPECT(out.find('z') != std::string::npos);
}

TEST_CASE("A wide character in the last column is not split across the edge") {
    Canvas canvas(3, 1);
    canvas.setWideChars(true);
    canvas.put(2, 0, U'\u4e16', {1, 1, 1}, {0, 0, 0});
    const auto out = canvas.flush();
    EXPECT(out.find("\xe4\xb8\x96") == std::string::npos); // Drawn as a blank.
}

TEST_CASE("A zero-width character on its own is drawn as a blank") {
    Canvas canvas(3, 1);
    canvas.setWideChars(true);
    canvas.put(0, 0, U'\u0301', {1, 1, 1}, {0, 0, 0});
    canvas.put(1, 0, U'a', {1, 1, 1}, {0, 0, 0});
    const auto out = canvas.flush();
    EXPECT(out.find("\xcc\x81") == std::string::npos);
    EXPECT(out.find(" a") != std::string::npos);
}

TEST_CASE("Without wide handling cells are one column each (fakeos)") {
    Canvas canvas(3, 1);
    canvas.put(0, 0, U'\u4e16', {1, 1, 1}, {0, 0, 0});
    canvas.put(1, 0, U'x', {1, 1, 1}, {0, 0, 0});
    EXPECT(canvas.flush().find("\xe4\xb8\x96x") != std::string::npos);
}

TEST_SUIT_END
