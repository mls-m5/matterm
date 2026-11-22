#include "mls-unit-test/unittest.h"
#include "wrapmark.h"

using namespace wrapmark;

TEST_SUIT_BEGIN(WrapMark)

TEST_CASE("The triangle is centred on the row, about 40 % of the cell high, "
          "inside its region") {
    const auto t = triangleFor(100.0f, 50.0f, 30, 6);
    EXPECT(t.has_value());
    // The base is a vertical edge at the left of the region, the point is on
    // the right.
    EXPECT_EQ(t->a.x, 100.0f);
    EXPECT_EQ(t->b.x, 100.0f);
    EXPECT_EQ(t->c.x, 100.0f + float(kStyle.width));
    EXPECT(t->c.x <= 100.0f + 6.0f);
    EXPECT_EQ(t->a.y, 50.0f + 15.0f - 6.0f); // 12 px high: 40 % of 30
    EXPECT_EQ(t->b.y, 50.0f + 15.0f + 6.0f);
    EXPECT_EQ(t->c.y, 65.0f);
}

TEST_CASE(
    "A small cell still gets a visible mark; it never taller than the cell") {
    const auto small = triangleFor(0, 0, 8, 6);
    EXPECT(small.has_value());
    EXPECT(small->b.y - small->a.y >= 4.0f);
    EXPECT(small->b.y - small->a.y <= 8.0f);
    const auto tiny = triangleFor(0, 0, 3, 6);
    EXPECT(tiny.has_value());
    EXPECT(tiny->b.y - tiny->a.y <= 3.0f);
}

TEST_CASE("No room, no mark") {
    EXPECT(!triangleFor(0, 0, 30, kStyle.width - 1).has_value());
    EXPECT(!triangleFor(0, 0, 0, 12).has_value());
    EXPECT(triangleFor(0, 0, 30, kStyle.width).has_value());
}

TEST_CASE("The strip: the thumb keeps its place without marks and gives room "
          "to them with") {
    const auto plain = stripLayout(12, false);
    EXPECT_EQ(plain.thumbX, 2);
    EXPECT_EQ(plain.thumbWidth, 8);
    EXPECT(!plain.hasMarks());

    const auto marked = stripLayout(12, true);
    EXPECT(marked.hasMarks());
    EXPECT(marked.markX >= 0);
    EXPECT(marked.markX + kStyle.width <=
           marked.thumbX); // The mark is left of the thumb, not under it.
    EXPECT(marked.thumbX + marked.thumbWidth <= 12);
    EXPECT(marked.thumbWidth >= 3);

    // A strip too narrow for both: no marks, the thumb as before.
    const auto narrow = stripLayout(8, true);
    EXPECT(!narrow.hasMarks());
    EXPECT_EQ(narrow.thumbX, 2);
    EXPECT_EQ(narrow.thumbWidth, 4);
}

TEST_SUIT_END
