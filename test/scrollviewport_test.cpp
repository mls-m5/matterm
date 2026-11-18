#include "mls-unit-test/unittest.h"
#include "scrollviewport.h"
#include "terminalmemory.h"

#include <cmath>
#include <limits>
#include <stdexcept>

TEST_SUIT_BEGIN(ScrollViewport)

TEST_CASE("Tail position and scrollbar proportions reflect history geometry") {
    ScrollViewport viewport;
    EXPECT_EQ(viewport.rowCount(), std::size_t(0));
    EXPECT_EQ(viewport.firstRow(), std::size_t(0));
    EXPECT_EQ(viewport.scrollbar().size, 1.0f);
    viewport.setGeometry(100, 10);
    EXPECT(viewport.atTail());
    EXPECT_EQ(viewport.firstRow(), std::size_t(90));
    EXPECT_EQ(viewport.rowCount(), std::size_t(10));
    EXPECT_EQ(viewport.scrollbar().position, 1.0f);
    EXPECT(std::abs(viewport.scrollbar().size - 0.1f) < 0.0001f);
    viewport.oldest();
    viewport.advance(10);
    EXPECT_EQ(viewport.firstRow(), std::size_t(0));
    EXPECT_EQ(viewport.scrollbar().position, 0.0f);
}

TEST_CASE(
    "Fractional scroll supplies an extra row and a negative pixel offset") {
    ScrollViewport viewport;
    viewport.setGeometry(100, 10);
    viewport.scrollBy(0.5);
    viewport.advance(10);
    EXPECT_EQ(viewport.offset(), 0.5);
    EXPECT_EQ(viewport.firstRow(), std::size_t(89));
    EXPECT_EQ(viewport.rowCount(), std::size_t(11));
    EXPECT_EQ(viewport.pixelOffset(20), -10.0f);
    viewport.followTail();
    viewport.advance(10);
    EXPECT(viewport.atTail());
    EXPECT_EQ(viewport.rowCount(), std::size_t(10));
}

TEST_CASE(
    "Smoothing converges without overshoot and is frame-rate independent") {
    ScrollViewport first;
    ScrollViewport second;
    first.setGeometry(100, 10);
    second.setGeometry(100, 10);
    first.scrollBy(20);
    second.scrollBy(20);
    first.advance(0.1);
    second.advance(0.05);
    second.advance(0.05);
    EXPECT(std::abs(first.offset() - second.offset()) < 0.000001);
    EXPECT_GT(first.offset(), 0.0);
    EXPECT_LT(first.offset(), 20.0);
    first.advance(10);
    EXPECT_EQ(first.offset(), 20.0);
}

TEST_CASE("New output preserves historical content across ring eviction") {
    TerminalMemory memory(1, 8, 3);
    for (char32_t ch = U'a'; ch <= U'h'; ++ch) {
        memory.append(std::u32string_view(&ch, 1));
    }
    ScrollViewport viewport;
    viewport.setGeometry(memory.size(), 2);
    viewport.scrollBy(2.5);
    viewport.advance(10);
    const auto character = memory.row(viewport.firstRow()).data()[0].codePoint;
    const auto pixels = viewport.pixelOffset(20);
    memory.append(U"i");
    viewport.rowsAppended(1, memory.size());
    EXPECT_EQ(viewport.offset(), 3.5);
    EXPECT(memory.row(viewport.firstRow()).data()[0].codePoint == character);
    EXPECT_EQ(viewport.pixelOffset(20), pixels);
    viewport.oldest();
    viewport.advance(10);
    memory.append(U"j");
    viewport.rowsAppended(1, memory.size());
    EXPECT_EQ(viewport.offset(), viewport.maxOffset());
    EXPECT_EQ(viewport.firstRow(), std::size_t(0));
}

TEST_CASE("Following output remains live and return animation keeps its "
          "destination") {
    ScrollViewport viewport;
    viewport.setGeometry(20, 10);
    viewport.rowsAppended(1, 21);
    EXPECT(viewport.atTail());
    viewport.scrollBy(5);
    viewport.advance(10);
    viewport.followTail();
    viewport.advance(0.01);
    const auto returningOffset = viewport.offset();
    viewport.rowsAppended(1, 22);
    EXPECT_EQ(viewport.targetOffset(), 0.0);
    EXPECT_EQ(viewport.offset(), returningOffset);
    // Continuous output must not keep pushing a returning camera away forever.
    for (int i = 0; i < 20; ++i) {
        viewport.rowsAppended(1, 23 + std::size_t(i));
        viewport.advance(0.1);
    }
    EXPECT(viewport.atTail());
}

TEST_CASE("Resize empty history and extreme input clamp safely") {
    ScrollViewport viewport;
    viewport.setGeometry(100, 10);
    viewport.scrollBy(std::numeric_limits<double>::max());
    viewport.advance(10);
    EXPECT_EQ(viewport.offset(), 90.0);
    viewport.setGeometry(12, 10);
    EXPECT_EQ(viewport.offset(), 2.0);
    viewport.setGeometry(0, 10);
    EXPECT(viewport.atTail());
    EXPECT_EQ(viewport.rowCount(), std::size_t(0));
    viewport.setGeometry(4, 0);
    EXPECT_EQ(viewport.rowCount(), std::size_t(0));
    EXPECT_THROW(viewport.scrollBy(std::numeric_limits<double>::infinity()),
                 std::invalid_argument);
    EXPECT_THROW(viewport.advance(-1), std::invalid_argument);
    EXPECT_THROW(viewport.advance(std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
}

TEST_CASE(
    "Tiny fractional scroll cannot round the pixel offset to a whole row") {
    ScrollViewport viewport;
    viewport.setGeometry(100, 10);
    viewport.scrollBy(0.000000001);
    viewport.advance(10);
    EXPECT_GT(viewport.pixelOffset(20), -20.0f);
    EXPECT(viewport.pixelOffset(20) <= 0.0f);
}

TEST_SUIT_END
