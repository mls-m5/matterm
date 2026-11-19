#include "mls-unit-test/unittest.h"
#include "windowsnap.h"

using frontend::snappedWindowSize;
using frontend::WindowSize;

TEST_SUIT_BEGIN(WindowSnap)

TEST_CASE("The window shrinks to whole cells plus the scrollbar strip") {
    // 960x640 window, cells 11x24, strip 12: 86 columns fit in 948, 26 rows.
    const auto size = snappedWindowSize({960, 640}, {960, 640}, 11, 24, 12);
    EXPECT_EQ(size.width, 86 * 11 + 12);
    EXPECT_EQ(size.height, 26 * 24);
}

TEST_CASE("An already snapped window is unchanged") {
    const WindowSize snapped{86 * 11 + 12, 26 * 24};
    EXPECT(snappedWindowSize(snapped, snapped, 11, 24, 12) == snapped);
}

TEST_CASE("HiDPI: the ratio converts back to window units") {
    // Drawable is twice the window; cells are 22x48 device pixels.
    const auto size = snappedWindowSize({500, 300}, {1000, 600}, 22, 48, 12);
    // (1000 - 12) / 22 = 44 columns -> 44*22+12 = 980 device px = 490 units;
    // 600 / 48 = 12 rows -> 576 device px = 288 units.
    EXPECT_EQ(size.width, 490);
    EXPECT_EQ(size.height, 288);
}

TEST_CASE("Tiny windows keep one cell and invalid input is returned as is") {
    const auto tiny = snappedWindowSize({5, 5}, {5, 5}, 11, 24, 12);
    EXPECT_EQ(tiny.width, 11 + 12);
    EXPECT_EQ(tiny.height, 24);
    const WindowSize bad{0, 0};
    EXPECT(snappedWindowSize(bad, {10, 10}, 11, 24, 12) == bad);
    EXPECT(snappedWindowSize({10, 10}, {10, 10}, 0, 24, 12) ==
           WindowSize({10, 10}));
}

TEST_SUIT_END
