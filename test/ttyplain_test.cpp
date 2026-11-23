#include "mls-unit-test/unittest.h"
#include "ttyplain.h"

#include <string>

namespace {
using namespace frontend::tty;

bool has(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_SUIT_BEGIN(TtyPlain)

TEST_CASE("The first picture clears the screen and writes every row, then the "
          "cursor") {
    ScreenImage image(4, 2);
    put(image, 0, 0, "ab");
    put(image, 0, 1, "cd");
    image.cursorVisible = true;
    image.cursorColumn = 2;
    image.cursorRow = 1;
    PlainPainter painter;
    const std::string out = painter.paint(image);
    EXPECT(has(out, "\x1b[2J"));
    EXPECT(has(out, "\x1b[1;1H"));
    EXPECT(has(out, "\x1b[2;1H"));
    EXPECT(has(out, "ab"));
    EXPECT(has(out, "cd"));
    EXPECT(has(out, "\x1b[2;3H\x1b[?25h"));
}

TEST_CASE("Nothing changed, nothing written") {
    ScreenImage image(4, 2);
    PlainPainter painter;
    painter.paint(image);
    EXPECT_EQ(painter.paint(image), std::string());
}

TEST_CASE("Only the rows that changed are written") {
    ScreenImage image(6, 3);
    put(image, 0, 0, "one");
    put(image, 0, 1, "two");
    put(image, 0, 2, "six");
    PlainPainter painter;
    painter.paint(image);
    put(image, 0, 1, "TWO");
    const std::string out = painter.paint(image);
    EXPECT(has(out, "\x1b[2;1H"));
    EXPECT(has(out, "TWO"));
    EXPECT(!has(out, "\x1b[1;1H") && !has(out, "\x1b[3;1H"));
    EXPECT(!has(out, "\x1b[2J"));
}

TEST_CASE("A move of the cursor alone is a short update") {
    ScreenImage image(10, 2);
    image.cursorVisible = true;
    PlainPainter painter;
    painter.paint(image);
    image.cursorColumn = 4;
    const std::string out = painter.paint(image);
    EXPECT(has(out, "\x1b[1;5H"));
    EXPECT(out.size() < 30);
}

TEST_CASE("A hidden cursor is hidden") {
    ScreenImage image(4, 1);
    image.cursorVisible = true;
    PlainPainter painter;
    painter.paint(image);
    image.cursorVisible = false;
    const std::string out = painter.paint(image);
    EXPECT(has(out, "\x1b[?25l"));
    EXPECT(!has(out, "\x1b[?25h"));
}

TEST_CASE("A new size starts over, and invalidate does too") {
    ScreenImage a(4, 2);
    PlainPainter painter;
    painter.paint(a);
    ScreenImage b(5, 3);
    EXPECT(has(painter.paint(b), "\x1b[2J"));
    painter.invalidate();
    EXPECT(has(painter.paint(b), "\x1b[2J"));
}

TEST_CASE("Colours are truecolor, or the nearest of 256, with the style bits") {
    ScreenImage image(2, 1);
    put(image,
        0,
        0,
        "x",
        Rgb8{255, 0, 0},
        Rgb8{0, 0, 255},
        CellAttribute::Bold | CellAttribute::Underline);
    PlainPainter truecolor;
    const std::string out = truecolor.paint(image);
    EXPECT(has(out, "\x1b[0;1;4;38;2;255;0;0;48;2;0;0;255m"));
    PlainPainter indexed(PlainCaps{false});
    const std::string out256 = indexed.paint(image);
    EXPECT(has(out256, "38;5;196;48;5;21m"));
}

TEST_CASE("The 256-colour match handles the greys and the corners") {
    EXPECT_EQ(nearest256(Rgb8{0, 0, 0}), 16);
    EXPECT_EQ(nearest256(Rgb8{255, 255, 255}), 231);
    EXPECT_EQ(nearest256(Rgb8{128, 128, 128}),
              244); // The grey ramp is closer than the cube.
    EXPECT_EQ(nearest256(Rgb8{0, 255, 0}), 46);
}

TEST_CASE("Characters are written as UTF-8 and nonsense becomes a space") {
    ScreenImage image(4, 1);
    put(image, 0, 0, "å€");
    image.at(2, 0).codePoint = char32_t(0xd800);
    image.at(3, 0).codePoint = char32_t(0x1f600);
    PlainPainter painter;
    const std::string out = painter.paint(image);
    EXPECT(has(out, "å€ "));
    EXPECT(has(out, "\xf0\x9f\x98\x80"));
}

TEST_CASE("Style codes are only repeated when the style changes") {
    ScreenImage image(6, 1);
    put(image, 0, 0, "abc");
    PlainPainter painter;
    const std::string out = painter.paint(image);
    std::size_t count = 0;
    for (std::size_t at = out.find("\x1b[0;38"); at != std::string::npos;
         at = out.find("\x1b[0;38", at + 1)) {
        ++count;
    }
    EXPECT_EQ(count, std::size_t(1));
}

TEST_SUIT_END
