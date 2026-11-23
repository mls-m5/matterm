#include "mls-unit-test/unittest.h"
#include "tty/width.h"

using tty::cellWidth;

TEST_SUIT_BEGIN(TtyWidth)

TEST_CASE("ASCII and Latin text is one column") {
    EXPECT_EQ(cellWidth(U'a'), 1);
    EXPECT_EQ(cellWidth(U' '), 1);
    EXPECT_EQ(cellWidth(U'å'), 1);
    EXPECT_EQ(cellWidth(U'€'), 1);
    EXPECT_EQ(cellWidth(U'─'), 1); // Box drawing is narrow.
}

TEST_CASE("East Asian wide characters and emoji are two columns") {
    EXPECT_EQ(cellWidth(U'世'), 2);         // CJK ideograph.
    EXPECT_EQ(cellWidth(U'あ'), 2);         // Hiragana.
    EXPECT_EQ(cellWidth(U'가'), 2);         // Hangul syllable.
    EXPECT_EQ(cellWidth(U'Ａ'), 2);         // Fullwidth A.
    EXPECT_EQ(cellWidth(U'\U0001f525'), 2); // Fire emoji.
    EXPECT_EQ(cellWidth(U'\U00020000'), 2); // CJK extension B.
}

TEST_CASE("Combining marks, joiners and controls take no column") {
    EXPECT_EQ(cellWidth(U'́'), 0);           // Combining acute.
    EXPECT_EQ(cellWidth(U'​'), 0);        // Zero width space.
    EXPECT_EQ(cellWidth(U'‍'), 0);        // Zero width joiner.
    EXPECT_EQ(cellWidth(U'️'), 0);        // Variation selector 16.
    EXPECT_EQ(cellWidth(U'\U000e0100'), 0); // Variation selector supplement.
    EXPECT_EQ(cellWidth(U'\0'), 0);
    EXPECT_EQ(cellWidth(U'\x1b'), 0);
    EXPECT_EQ(cellWidth(U'\x7f'), 0);
}

TEST_SUIT_END
