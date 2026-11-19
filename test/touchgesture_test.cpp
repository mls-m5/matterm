#include "mls-unit-test/unittest.h"
#include "touchgesture.h"

using frontend::TouchGesture;
using Kind = TouchGesture::Kind;

TEST_SUIT_BEGIN(TouchGesture)

TEST_CASE("A short touch without movement is a tap") {
    TouchGesture g;
    EXPECT(g.down(1, 50, 60, 0).empty());
    EXPECT(g.tick(100).empty());
    const auto out = g.up(1, 120);
    EXPECT_EQ(out.size(), std::size_t(1));
    EXPECT(out[0].kind == Kind::Tap);
    EXPECT_EQ(out[0].x, 50.0);
    EXPECT(!g.active());
}

TEST_CASE("Small jitter stays a tap") {
    TouchGesture g;
    g.down(1, 50, 60, 0);
    EXPECT(g.move(1, 54, 63, 10).empty());
    EXPECT(g.up(1, 20)[0].kind == Kind::Tap);
}

TEST_CASE("Dragging past the slop scrolls by the distance moved") {
    TouchGesture g;
    g.down(1, 50, 100, 0);
    auto out = g.move(1, 50, 130, 10);
    EXPECT_EQ(out.size(), std::size_t(1));
    EXPECT(out[0].kind == Kind::Scroll);
    EXPECT_EQ(out[0].amount, 30.0);
    out = g.move(1, 52, 120, 20);
    EXPECT_EQ(out[0].amount, -10.0);
    EXPECT(g.up(1, 30).empty());  // No tap after a drag.
    EXPECT(g.tick(1000).empty()); // And no long press.
}

TEST_CASE("A held finger becomes a long press, then selects") {
    TouchGesture g;
    g.down(1, 20, 30, 1000);
    EXPECT(g.tick(1449).empty());
    EXPECT_EQ(g.longPressDue(), std::uint64_t(1450));
    auto out = g.tick(1450);
    EXPECT_EQ(out.size(), std::size_t(1));
    EXPECT(out[0].kind == Kind::LongPress);
    EXPECT(g.tick(2000).empty()); // Reported once.
    out = g.move(1, 80, 30, 1500);
    EXPECT(out[0].kind == Kind::SelectExtend);
    EXPECT_EQ(out[0].x, 80.0);
    out = g.up(1, 1600);
    EXPECT(out[0].kind == Kind::SelectEnd);
    EXPECT(!g.active());
}

TEST_CASE("Moving before the long-press time cancels it") {
    TouchGesture g;
    g.down(1, 20, 30, 0);
    g.move(1, 20, 80, 100);
    EXPECT(g.tick(1000).empty());
    EXPECT_EQ(g.longPressDue(), std::uint64_t(0));
}

TEST_CASE("Two fingers pinch in whole font steps") {
    TouchGesture g({10, 450, 20});
    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 5);                  // 100 apart.
    EXPECT(g.move(2, 215, 100, 10).empty()); // +15: not a step yet.
    auto out = g.move(2, 245, 100, 20);      // +45: two steps.
    EXPECT_EQ(out.size(), std::size_t(1));
    EXPECT(out[0].kind == Kind::Pinch);
    EXPECT_EQ(out[0].amount, 2.0);
    out = g.move(2, 200, 100, 30); // Back to 100 apart: two steps down.
    EXPECT_EQ(out[0].amount, -2.0);
    out = g.move(2, 150, 100, 40); // Fingers closer: negative steps.
    EXPECT(out[0].amount < 0);
}

TEST_CASE("A second finger cancels a long press selection and a scroll") {
    TouchGesture g;
    g.down(1, 10, 10, 0);
    g.tick(500);
    auto out = g.down(2, 100, 10, 600);
    EXPECT_EQ(out.size(), std::size_t(1));
    EXPECT(out[0].kind == Kind::SelectEnd);
    TouchGesture h;
    h.down(1, 10, 10, 0);
    h.move(1, 10, 60, 10);
    EXPECT(h.down(2, 100, 60, 20).empty());
    EXPECT(
        h.move(1, 10, 100, 30).empty()); // Distance changed less than a step.
}

TEST_CASE("After a pinch the remaining finger neither scrolls nor taps") {
    TouchGesture g;
    g.down(1, 10, 10, 0);
    g.down(2, 100, 10, 5);
    EXPECT(g.up(2, 10).empty());
    EXPECT(g.active());
    EXPECT(g.move(1, 10, 200, 20).empty());
    EXPECT(g.up(1, 30).empty());
    EXPECT(!g.active());
    g.down(3, 5, 5, 100); // A new gesture works again.
    EXPECT(g.up(3, 110)[0].kind == Kind::Tap);
}

TEST_CASE("Cancel ends a selection and resets") {
    TouchGesture g;
    g.down(1, 10, 10, 0);
    g.tick(500);
    EXPECT(g.cancel()[0].kind == Kind::SelectEnd);
    EXPECT(!g.active());
    EXPECT(g.cancel().empty());
}

TEST_CASE("Font steps are clamped and scaled by the pixel ratio") {
    EXPECT_EQ(frontend::steppedFontSize(18, 3), 21);
    EXPECT_EQ(frontend::steppedFontSize(10, -9), 8);
    EXPECT_EQ(frontend::steppedFontSize(39, 5), 40);
    EXPECT_EQ(frontend::fontDevicePixels(18, 2.0), 36);
    EXPECT_EQ(frontend::fontDevicePixels(18, 2.625), 47);
    EXPECT_EQ(frontend::fontDevicePixels(8, 1.0), 8);
}

TEST_SUIT_END
