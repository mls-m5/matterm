#include "mls-unit-test/unittest.h"
#include "palette.h"
#include "terminalmodel.h"
#include "vtparser.h"

TEST_SUIT_BEGIN(Palette)

TEST_CASE(
    "Cell layout remains exactly twelve bytes and legacy RGB is unchanged") {
    EXPECT_EQ(sizeof(FChar), std::size_t(12));
    FChar cell{U'A', {1, 2, 3}, {4, 5, 6}};
    EXPECT(cell.foregroundColor().kind == ColorReference::Kind::Rgb);
    EXPECT_EQ(Palette{}.resolve(cell).foreground.g, 2);
    cell.setForeground(ColorReference::indexed(255));
    cell.setBackground(ColorReference::defaultColor());
    cell.setStyle(CellAttribute::Bold | CellAttribute::Underline);
    EXPECT_EQ(cell.foregroundColor().value.r, 255);
    EXPECT(cell.backgroundColor().kind == ColorReference::Kind::Default);
    EXPECT(hasAttribute(cell.attributes, CellAttribute::Bold));
}

TEST_CASE(
    "Theme edits recolor existing indexed and default cells not explicit RGB") {
    TerminalModel model(8, 2);
    VtParser parser(model);
    parser.feed("A\x1b[31mB\x1b[38;2;1;2;3mC\x1b[0mD");
    Palette palette;
    EXPECT(model.cell(0, 0).foregroundColor().kind ==
           ColorReference::Kind::Default);
    EXPECT(model.cell(1, 0).foregroundColor().kind ==
           ColorReference::Kind::Indexed);
    EXPECT(model.cell(2, 0).foregroundColor().kind ==
           ColorReference::Kind::Rgb);
    palette.foreground = {10, 20, 30};
    palette.background = {40, 50, 60};
    palette.colors[1] = {70, 80, 90};
    EXPECT_EQ(palette.resolve(model.cell(0, 0)).foreground.r, 10);
    EXPECT_EQ(palette.resolve(model.cell(1, 0)).foreground.r, 70);
    EXPECT_EQ(palette.resolve(model.cell(2, 0)).foreground.r, 1);
    EXPECT_EQ(palette.resolve(model.cell(3, 0)).background.r, 40);
    model.eraseLine(TerminalModel::EraseMode::All);
    EXPECT(model.cell(7, 0).backgroundColor().kind ==
           ColorReference::Kind::Default);
}

TEST_CASE("Inverse conceal faint and save restore preserve color references") {
    TerminalModel model(8, 2);
    VtParser parser(model);
    parser.feed("\x1b[31;7;2mA\x1b"
                "7\x1b[0m!\x1b"
                "8B");
    Palette palette;
    palette.colors[1] = {100, 120, 140};
    palette.background = {20, 40, 60};
    const auto colors = palette.resolve(model.cell(0, 0));
    EXPECT_EQ(colors.background.r, 100);
    EXPECT_EQ(colors.foreground.r, 60);
    EXPECT_EQ(palette.resolve(model.cell(1, 0)).foreground.g, 80);
    parser.feed("\x1b[?1049h\x1b[0mX\x1b[?1049lC");
    EXPECT_EQ(model.cell(2, 0).foregroundColor().value.r, 1);
    EXPECT(hasAttribute(model.cell(2, 0).attributes, CellAttribute::Inverse));
}

TEST_CASE(
    "Default palette has dark background a full color cube and gray ramp") {
    Palette palette;
    EXPECT_EQ(palette.background.r, 30);
    EXPECT_EQ(palette.colors[16].r, 0);
    EXPECT_EQ(palette.colors[196].r, 255);
    EXPECT_EQ(palette.colors[231].b, 255);
    EXPECT_EQ(palette.colors[232].g, 8);
    EXPECT_EQ(palette.colors[255].b, 238);
}

TEST_SUIT_END
