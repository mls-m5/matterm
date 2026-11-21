#include "cellbuffer.h"
#include "framesignature.h"
#include "mls-unit-test/unittest.h"

#include <vector>

namespace {
using frontend::DividerBox;
using frontend::PaneSignatureInput;
using frontend::PixelRect;
using frontend::windowSignature;

struct Pane {
    CellBuffer cells{4, 2};
    Palette palette;
    RenderFrame frame{cells.view(), {}};
    PixelRect rect;
    PixelRect scrollbar;
    PaneSignatureInput input() {
        frame.cells = cells.view();
        return {&frame, &palette, rect, scrollbar};
    }
};
} // namespace

TEST_SUIT_BEGIN(FrameSignature)

TEST_CASE("One frame: any visible change changes the signature") {
    Pane p;
    const auto base = frontend::frameSignature(p.frame, p.palette);
    EXPECT_EQ(base, frontend::frameSignature(p.frame, p.palette));
    p.cells.at(1, 1).codePoint = U'x';
    EXPECT(frontend::frameSignature(p.frame, p.palette) != base);
}

TEST_CASE(
    "A window signature covers every pane, divider, place and the strip") {
    Pane a, b, strip;
    a.rect = {0, 20, 100, 100};
    b.rect = {110, 20, 100, 100};
    strip.rect = {0, 0, 210, 20};
    DividerBox divider;
    divider.rect = {100, 20, 10, 100};
    auto sign = [&] {
        return windowSignature(
            {strip.input(), a.input(), b.input()}, {divider}, 210, 120);
    };
    const auto base = sign();
    EXPECT_EQ(base, sign());           // Idle: stable.
    b.cells.at(3, 1).codePoint = U'z'; // The second pane changed.
    const auto second = sign();
    EXPECT(second != base);
    strip.cells.at(0, 0).codePoint = U'T'; // The tab strip changed.
    const auto third = sign();
    EXPECT(third != second);
    divider.rect.x = 101; // A divider moved.
    const auto fourth = sign();
    EXPECT(fourth != third);
    a.rect.w = 99; // A pane moved or resized.
    const auto fifth = sign();
    EXPECT(fifth != fourth);
    a.scrollbar = {96, 20, 4, 100};
    EXPECT(sign() != fifth); // Its scrollbar place.
    a.frame.scrollbar = {true, 0.5f, 0.2f};
    const auto sixth = sign();
    a.frame.scrollbar.position = 0.6f;
    EXPECT(sign() != sixth); // Its scrollbar state.
    EXPECT(windowSignature({a.input()}, {}, 1, 1) !=
           windowSignature({a.input()}, {}, 2, 1)); // The window size.
}

TEST_SUIT_END
