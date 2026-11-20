#include "cursorglide.h"
#include "mls-unit-test/unittest.h"

#include <cmath>

TEST_SUIT_BEGIN(CursorGlide)

TEST_CASE("The first position and hidden cursors snap") {
    CursorGlide glide;
    glide.update(5, 2, true, 0.0);
    EXPECT_EQ(glide.columnOffset(), 0.0f);
    EXPECT(!glide.animating());
    glide.update(9, 2, false, 0.1); // Hidden: no glide from stale position.
    glide.update(9, 2, true, 0.2);
    EXPECT_EQ(glide.columnOffset(), 0.0f);
}

TEST_CASE("A move glides from the old cell and settles at the target") {
    CursorGlide::Config config;
    config.durationSeconds = 0.1;
    CursorGlide glide(config);
    glide.update(0, 0, true, 0.0);
    glide.update(4, 0, true, 1.0); // Target moved 4 cells right.
    // Just after the move the cursor is still near the old cell: offset is
    // (shown - target), about -4 cells.
    EXPECT(glide.columnOffset() < -3.9f);
    EXPECT(glide.animating());
    glide.update(4, 0, true, 1.05);
    const float middle = glide.columnOffset();
    EXPECT(middle < -0.5f && middle > -3.5f);
    glide.update(4, 0, true, 1.2);
    EXPECT_EQ(glide.columnOffset(), 0.0f);
    EXPECT(!glide.animating());
}

TEST_CASE("Retargeting mid-glide continues from the position shown") {
    CursorGlide::Config config;
    config.durationSeconds = 0.1;
    CursorGlide glide(config);
    glide.update(0, 0, true, 0.0);
    glide.update(10, 0, true, 1.0);
    glide.update(10, 0, true, 1.05);
    const float shown = 10 + glide.columnOffset(); // Absolute position now.
    glide.update(10, 3, true, 1.05);               // New target row.
    // Same absolute column right after the retarget; no jump.
    EXPECT(std::abs((10 + double(glide.columnOffset())) - double(shown)) < 1e-4);
    EXPECT(glide.rowOffset() <= 0.0f);
}

TEST_CASE("Long jumps snap, and a disabled glide never offsets") {
    CursorGlide::Config config;
    config.maxGlideCells = 5;
    CursorGlide glide(config);
    glide.update(0, 0, true, 0.0);
    glide.update(40, 10, true, 1.0);
    EXPECT_EQ(glide.columnOffset(), 0.0f);
    EXPECT_EQ(glide.rowOffset(), 0.0f);

    CursorGlide::Config off;
    off.enabled = false;
    CursorGlide plain(off);
    plain.update(0, 0, true, 0.0);
    plain.update(1, 1, true, 0.01);
    EXPECT_EQ(plain.columnOffset(), 0.0f);
    EXPECT(!plain.animating());
}

TEST_CASE("apply writes offsets and leaves the true cell alone") {
    CursorGlide glide;
    glide.update(0, 0, true, 0.0);
    glide.update(3, 1, true, 1.0);
    CursorState cursor;
    cursor.column = 3;
    cursor.row = 1;
    glide.apply(cursor);
    EXPECT_EQ(cursor.column, 3);
    EXPECT_EQ(cursor.row, 1);
    EXPECT(cursor.columnOffset < 0.0f);
}

TEST_SUIT_END
