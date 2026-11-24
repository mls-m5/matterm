#include "mls-unit-test/unittest.h"
#include "splitdecoration.h"
#include "themes.h"
using namespace frontend;
namespace {
int pixel(const SplitDecoration &plan, int x, int y) {
    int color = -1;
    const auto contains = [&](const PixelRect &r) {
        return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
    };
    for (const auto &r : plan.edges)
        if (contains(r))
            color = 0;
    for (const auto &r : plan.middle)
        if (contains(r))
            color = 1;
    for (const auto &r : plan.outer)
        if (contains(r))
            color = 0;
    return color;
}
} // namespace
TEST_SUIT_BEGIN(SplitDecoration)
TEST_CASE(
    "T and cross junctions have solid themed middles without dark seams") {
    const DividerBox vertical{{10, 0, 6, 30}, SplitDirection::SideBySide};
    const DividerBox arm{{16, 10, 24, 6}, SplitDirection::Stacked};
    const auto tee =
        splitDecoration({vertical, arm}, {0, 0, 40, 30}, true, false, false);
    EXPECT(pixel(tee, 10, 5) == 0);
    EXPECT(pixel(tee, 11, 5) == 1);
    EXPECT(pixel(tee, 15, 12) == 1); // No vertical dark stripe through the T.
    EXPECT(pixel(tee, 20, 10) == 0);
    EXPECT(pixel(tee, 20, 11) == 1);
    EXPECT(pixel(tee, 9, 9) == -1);
    const DividerBox crossArm{{0, 10, 40, 6}, SplitDirection::Stacked};
    const auto cross = splitDecoration(
        {vertical, crossArm}, {0, 0, 40, 30}, true, false, false);
    for (int y = 10; y < 16; ++y)
        for (int x = 10; x < 16; ++x)
            EXPECT(pixel(cross, x, y) == 1);
    EXPECT(vertical.rect == PixelRect({10, 0, 6, 30})); // No layout mutation.
    const auto left =
        splitDecoration({{{20, 0, 6, 40}, SplitDirection::SideBySide},
                         {{0, 10, 20, 6}, SplitDirection::Stacked}},
                        {0, 0, 40, 40},
                        true,
                        false,
                        false);
    EXPECT(pixel(left, 20, 12) == 1);
    const auto above =
        splitDecoration({{{10, 0, 6, 20}, SplitDirection::SideBySide},
                         {{0, 20, 40, 6}, SplitDirection::Stacked}},
                        {0, 0, 40, 40},
                        true,
                        false,
                        false);
    EXPECT(pixel(above, 12, 20) == 1);
    const auto below =
        splitDecoration({{{10, 26, 6, 14}, SplitDirection::SideBySide},
                         {{0, 20, 40, 6}, SplitDirection::Stacked}},
                        {0, 0, 40, 40},
                        true,
                        false,
                        false);
    EXPECT(pixel(below, 12, 25) == 1);
}
TEST_CASE("Outer edges avoid own-frame double borders and screen edges in "
          "maximized/fullscreen") {
    const PixelRect content{0, 5, 40, 25};
    const auto ordinary = splitDecoration({}, content, false, false, false);
    EXPECT(pixel(ordinary, 0, 12) == 0 && pixel(ordinary, 39, 12) == 0);
    EXPECT(pixel(ordinary, 20, 5) == 0 && pixel(ordinary, 20, 29) == 0);
    const auto own = splitDecoration({}, content, true, false, false);
    EXPECT(pixel(own, 0, 12) == -1 && pixel(own, 20, 29) == -1);
    EXPECT(pixel(own, 20, 5) ==
           0); // Thin seam below the strip, not a second window frame.
    for (const bool fullscreen : {false, true}) {
        const auto edge =
            splitDecoration({}, content, false, !fullscreen, fullscreen);
        EXPECT(pixel(edge, 0, 12) == -1 && pixel(edge, 20, 29) == -1);
        EXPECT(pixel(edge, 20, 5) == 0);
        EXPECT(
            splitDecoration({}, {0, 0, 40, 30}, false, !fullscreen, fullscreen)
                .outer.empty());
    }
}
TEST_CASE(
    "One-pixel, clipped and tiny geometry never creates invalid rectangles") {
    for (const int size : {0, 1, 2, 3, 12}) {
        const auto plan =
            splitDecoration({{{-2, 0, 3, 40}, SplitDirection::SideBySide}},
                            {0, 0, size, size},
                            false,
                            false,
                            false);
        for (const auto *layer : {&plan.edges, &plan.middle, &plan.outer})
            for (const auto &r : *layer)
                EXPECT(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
                       r.x + r.w <= size && r.y + r.h <= size);
    }
}
TEST_CASE("High-DPI drawable geometry retains exactly one physical pixel of "
          "dark edge") {
    const auto plan =
        splitDecoration({{{20, 0, 12, 60}, SplitDirection::SideBySide}},
                        {0, 0, 80, 60},
                        true,
                        false,
                        false);
    EXPECT(pixel(plan, 20, 10) == 0);
    EXPECT(pixel(plan, 21, 10) == 1);
    EXPECT(pixel(plan, 30, 10) == 1);
    EXPECT(pixel(plan, 31, 10) == 0);
}

TEST_CASE(
    "Middle and dark edge derive from the current strip palette on each draw") {
    const auto blue = themes::stripPalette(themes::theme("blue"));
    const auto green = themes::stripPalette(themes::theme("green"));
    const auto same = [](Rgb8 a, Rgb8 b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };
    EXPECT(same(splitMiddleColor(blue), borderColor(blue)));
    EXPECT(!same(splitMiddleColor(blue), splitMiddleColor(green)));
    const auto edge = splitEdgeColor(blue), middle = splitMiddleColor(blue);
    EXPECT(edge.r <= middle.r && edge.g <= middle.g && edge.b <= middle.b);
}
TEST_SUIT_END
