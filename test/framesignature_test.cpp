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

TEST_CASE("The wrapped-line marks are part of what is drawn") {
    Pane p;
    const auto none = frontend::frameSignature(p.frame, p.palette);
    p.frame.wrapMarks = {false, false};
    const auto enabled = frontend::frameSignature(p.frame, p.palette);
    EXPECT(enabled != none); // Marks on, none set: the thumb moves aside.
    p.frame.wrapMarks = {true, false};
    const auto first = frontend::frameSignature(p.frame, p.palette);
    EXPECT(first != enabled);
    p.frame.wrapMarks = {false, true};
    EXPECT(frontend::frameSignature(p.frame, p.palette) != first);
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

TEST_CASE("Blinking changes the drawn signature but not the steady one (a resting window sleeps)") {
    Pane p;
    p.frame.cursor.visible = true;
    p.frame.cursor.blink = true;
    const auto signatures = [&](bool blinkOn, bool cursorShown) {
        p.frame.textBlinkVisible = blinkOn;
        p.frame.cursor.visible = cursorShown;
        frontend::PaneSignatureInputEx in;
        in.frame = &p.frame;
        in.palette = &p.palette;
        in.rect = {0, 0, 40, 20};
        in.steadyCursorVisible = true; // The cursor is there; blinking only hides it for a moment.
        return frontend::windowSignatures({in}, {});
    };
    const auto on = signatures(true, true);
    const auto off = signatures(false, false);
    EXPECT(on.full != off.full);     // The pixels differ: draw.
    EXPECT_EQ(on.steady, off.steady); // Nothing happened: no reason to stay awake.
    // A cell change is a change of both.
    p.cells.at(2, 0).codePoint = U'q';
    const auto typed = signatures(true, true);
    EXPECT(typed.full != on.full);
    EXPECT(typed.steady != on.steady);
    // A cursor that moved, or that is hidden by the program, is a change of the steady one too.
    p.frame.cursor.column = 3;
    EXPECT(signatures(true, true).steady != typed.steady);
    frontend::PaneSignatureInputEx hidden;
    hidden.frame = &p.frame;
    hidden.palette = &p.palette;
    hidden.rect = {0, 0, 40, 20};
    hidden.steadyCursorVisible = false;
    EXPECT(frontend::windowSignatures({hidden}, {}).steady != signatures(true, true).steady);
}

TEST_CASE("The hash sees every byte of a long row, also the tail that is not a whole word") {
    frontend::FrameSignature a, b;
    std::vector<unsigned char> bytes(37, 7);
    a.bytes(bytes.data(), bytes.size());
    bytes[36] = 8;
    b.bytes(bytes.data(), bytes.size());
    EXPECT(a.value() != b.value());
    bytes[36] = 7;
    bytes[0] = 9;
    frontend::FrameSignature c;
    c.bytes(bytes.data(), bytes.size());
    EXPECT(c.value() != a.value());
}

TEST_SUIT_END
