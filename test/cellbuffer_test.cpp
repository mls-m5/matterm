#include "cellbuffer.h"
#include "mls-unit-test/unittest.h"

#include <limits>

TEST_SUIT_BEGIN(CellBuffer)

TEST_CASE("Cells start blank and coordinates are checked") {
    CellBuffer buffer(3, 2);
    EXPECT_EQ(buffer.columns(), std::size_t(3));
    EXPECT_EQ(buffer.rows(), std::size_t(2));
    EXPECT(buffer.at(2, 1).codePoint == U' ');
    const CellBuffer &readOnly = buffer;
    EXPECT_EQ(readOnly.at(0, 0).foreground.r, 255);
    EXPECT_EQ(readOnly.at(0, 0).background.r, 0);
    EXPECT_THROW(buffer.at(3, 0), std::out_of_range);
    EXPECT_THROW(readOnly.at(0, 2), std::out_of_range);
}

TEST_CASE("Drawing code points preserves colors and clips without wrapping") {
    CellBuffer buffer(4, 2);
    buffer.draw(1, 0, U"abcd", {1, 2, 3}, {4, 5, 6});
    EXPECT(buffer.at(0, 0).codePoint == U' ');
    EXPECT(buffer.at(1, 0).codePoint == U'a');
    EXPECT(buffer.at(3, 0).codePoint == U'c');
    EXPECT(buffer.at(0, 1).codePoint == U' ');
    EXPECT_EQ(buffer.at(1, 0).foreground.g, 2);
    EXPECT_EQ(buffer.at(1, 0).background.b, 6);
    buffer.draw(-2, 1, U"abcde");
    EXPECT(buffer.at(0, 1).codePoint == U'c');
    EXPECT(buffer.at(2, 1).codePoint == U'e');
    EXPECT(buffer.at(3, 1).codePoint == U' ');
}

TEST_CASE(
    "Empty and offscreen draws do nothing including extreme coordinates") {
    CellBuffer buffer(2, 2);
    buffer.draw(0, 0, U"");
    buffer.draw(0, -1, U"x");
    buffer.draw(0, 2, U"x");
    buffer.draw(2, 0, U"x");
    buffer.draw(std::numeric_limits<int>::min(), 0, U"x");
    buffer.draw(std::numeric_limits<int>::max(), 0, U"x");
    buffer.draw(0, 0, FStringView{});
    EXPECT(buffer.at(0, 0).codePoint == U' ');
    EXPECT(buffer.at(1, 1).codePoint == U' ');
}

TEST_CASE("Formatted draws preserve cells and support overlapping input") {
    CellBuffer buffer(5, 1);
    FString text{FChar{U'A', {10, 20, 30}, {40, 50, 60}}, FChar{U'B'}};
    buffer.draw(0, 0, FStringView(text));
    EXPECT_EQ(buffer.at(0, 0).foreground.g, 20);
    EXPECT_EQ(buffer.at(0, 0).background.b, 60);
    buffer.draw(0, 0, U"abcde");
    buffer.draw(1, 0, buffer.view().row(0));
    EXPECT(buffer.at(1, 0).codePoint == U'a');
    EXPECT(buffer.at(4, 0).codePoint == U'd');
    buffer.draw(-1, 0, buffer.view().row(0));
    EXPECT(buffer.at(0, 0).codePoint == U'a');
    EXPECT(buffer.at(3, 0).codePoint == U'd');
}

TEST_CASE("Borrowed views see edits and clearing") {
    CellBuffer buffer(2, 2);
    const auto view = buffer.view();
    buffer.draw(1, 1, U"ä");
    EXPECT(view.row(1).data()[1].codePoint == U'ä');
    buffer.clear(FChar{U'.', {1, 2, 3}, {4, 5, 6}});
    EXPECT(view.row(1).data()[1].codePoint == U'.');
    EXPECT_EQ(view.row(0).data()[0].background.r, 4);
    buffer.clear();
    EXPECT(view.row(0).data()[0].codePoint == U' ');
}

TEST_CASE("Resize preserves the top-left intersection and fills new cells") {
    CellBuffer buffer(2, 2);
    buffer.draw(0, 0, U"ab");
    buffer.draw(0, 1, U"cd");
    buffer.resize(3, 3, FChar{U'.'});
    EXPECT(buffer.at(1, 1).codePoint == U'd');
    EXPECT(buffer.at(2, 0).codePoint == U'.');
    EXPECT(buffer.at(0, 2).codePoint == U'.');
    EXPECT(buffer.view().row(1).data()[1].codePoint == U'd');
    buffer.resize(1, 2);
    EXPECT(buffer.at(0, 0).codePoint == U'a');
    EXPECT(buffer.at(0, 1).codePoint == U'c');
    EXPECT_EQ(buffer.view().row(0).size(), std::size_t(1));
}

TEST_CASE("Zero-sized surfaces can be cleared drawn and resized") {
    CellBuffer buffer(0, 2);
    buffer.clear();
    buffer.draw(0, 0, U"x");
    EXPECT_EQ(buffer.view().row(1).size(), std::size_t(0));
    EXPECT_THROW(buffer.at(0, 0), std::out_of_range);
    buffer.resize(2, 0);
    EXPECT_EQ(buffer.view().rows(), std::size_t(0));
    buffer.resize(1, 1);
    EXPECT(buffer.at(0, 0).codePoint == U' ');
}

TEST_CASE("Impossible dimensions leave the original buffer intact") {
    CellBuffer buffer(2, 1);
    buffer.draw(0, 0, U"ok");
    const auto view = buffer.view();
    EXPECT_THROW(buffer.resize(std::numeric_limits<std::size_t>::max(), 2),
                 std::length_error);
    EXPECT_EQ(buffer.columns(), std::size_t(2));
    EXPECT(view.row(0).data()[1].codePoint == U'k');
}

TEST_SUIT_END
