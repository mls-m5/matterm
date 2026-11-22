#include "mls-unit-test/unittest.h"
#include "tabstrip.h"
#include "windowhit.h"

#include <vector>

namespace {
using namespace frontend;
using Zone = WindowZone;

// A 1000x600 window, a 16 px strip, and a drag handle between x=300 and x=700.
WindowGeometry window() {
    WindowGeometry g;
    g.width = 1000;
    g.height = 600;
    g.stripHeight = 16;
    g.drag = {{300, 700}};
    return g;
}
} // namespace

TEST_SUIT_BEGIN(WindowHit)

TEST_CASE("The edges and corners resize, the inside does not") {
    const WindowGeometry g = window();
    EXPECT(classifyWindowPoint(g, 500, 300) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 0, 300) == Zone::ResizeLeft);
    EXPECT(classifyWindowPoint(g, 999, 300) == Zone::ResizeRight);
    EXPECT(classifyWindowPoint(g, 500, 599) == Zone::ResizeBottom);
    EXPECT(classifyWindowPoint(g, 500, 0) == Zone::ResizeTop);
    EXPECT(classifyWindowPoint(g, 0, 0) == Zone::ResizeTopLeft);
    EXPECT(classifyWindowPoint(g, 999, 0) == Zone::ResizeTopRight);
    EXPECT(classifyWindowPoint(g, 0, 599) == Zone::ResizeBottomLeft);
    EXPECT(classifyWindowPoint(g, 999, 599) == Zone::ResizeBottomRight);
    // The edge is kResizeEdge wide; a corner reaches kResizeCorner along the
    // edge.
    EXPECT(classifyWindowPoint(g, kResizeEdge - 1, 300) == Zone::ResizeLeft);
    EXPECT(classifyWindowPoint(g, kResizeEdge, 300) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 0, kResizeCorner - 1) == Zone::ResizeTopLeft);
    EXPECT(classifyWindowPoint(g, 0, kResizeCorner) == Zone::ResizeLeft);
    EXPECT(classifyWindowPoint(g, kResizeCorner - 1, 0) == Zone::ResizeTopLeft);
    EXPECT(classifyWindowPoint(g, kResizeCorner, 0) == Zone::ResizeTop);
    EXPECT(classifyWindowPoint(g, 999 - (kResizeCorner - 1), 599) ==
           Zone::ResizeBottomRight);
}

TEST_CASE("Only gaps of the strip drag, and the top edge wins over the strip") {
    const WindowGeometry g = window();
    EXPECT(classifyWindowPoint(g, 500, 8) == Zone::Drag);
    EXPECT(classifyWindowPoint(g, 300, 8) == Zone::Drag);
    EXPECT(classifyWindowPoint(g, 299, 8) == Zone::Normal);  // A tab.
    EXPECT(classifyWindowPoint(g, 700, 8) == Zone::Normal);  // A button.
    EXPECT(classifyWindowPoint(g, 500, 16) == Zone::Normal); // Below the strip.
    EXPECT(classifyWindowPoint(g, 500, 2) == Zone::ResizeTop);
    EXPECT(classifyWindowPoint(g, 500, kResizeEdge) == Zone::Drag);
}

TEST_CASE("A maximised window has no resize edges but still drags and gives "
          "the buttons clicks") {
    WindowGeometry g = window();
    g.maximized = true;
    EXPECT(classifyWindowPoint(g, 0, 300) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 500, 0) == Zone::Drag);
    EXPECT(classifyWindowPoint(g, 999, 599) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 100, 0) == Zone::Normal);
}

TEST_CASE(
    "Points outside the window and tiny windows give something sensible") {
    WindowGeometry g = window();
    EXPECT(classifyWindowPoint(g, -1, 10) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 1000, 10) == Zone::Normal);
    EXPECT(classifyWindowPoint(g, 10, 600) == Zone::Normal);
    WindowGeometry tiny;
    tiny.width = 10;
    tiny.height = 10;
    tiny.stripHeight = 16; // Taller than the window.
    tiny.drag = {{0, 10}};
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            (void)classifyWindowPoint(tiny, x, y); // No crash, no hang.
        }
    }
    WindowGeometry empty;
    EXPECT(classifyWindowPoint(empty, 0, 0) == Zone::Normal);
}

TEST_CASE(
    "The drag map is the strip's columns without a region, in window pixels") {
    // Two tabs, then a gap, then the palette button and the three window
    // buttons.
    std::vector<TabInfo> tabs(2);
    tabs[0].title = "a";
    tabs[1].title = "b";
    tabs[0].active = true;
    const TabStrip strip = buildTabStrip(tabs,
                                         80,
                                         Palette(),
                                         0,
                                         true,
                                         ShareButton{true, 0},
                                         WindowButtons{true, false});
    const auto spans = dragSpans(strip, 8, 1.0);
    EXPECT(!spans.empty());
    for (const auto &s : spans) {
        EXPECT(s.begin < s.end);
        for (int px = s.begin; px < s.end; ++px) {
            EXPECT(strip.hit(px / 8).kind == StripHit::Kind::None);
        }
    }
    // Every column without a region is in some span.
    for (int column = 0; column < int(strip.cells.size()); ++column) {
        if (strip.hit(column).kind != StripHit::Kind::None)
            continue;
        bool in = false;
        for (const auto &s : spans)
            in = in || (column * 8 >= s.begin && column * 8 < s.end);
        EXPECT(in);
    }
    // The gap before Share is wide and contiguous; a tab and the buttons are
    // not in it.
    int widest = 0;
    for (const auto &s : spans)
        widest = std::max(widest, s.end - s.begin);
    EXPECT_GT(widest, 8 * 10);
    // High DPI: 2 drawable pixels per window pixel halves the numbers.
    const auto half = dragSpans(strip, 8, 2.0);
    EXPECT_EQ(half.size(), spans.size());
    EXPECT_EQ(half.front().begin, spans.front().begin / 2);
    EXPECT_EQ(half.front().end, spans.front().end / 2);
    // Without regions nothing drags (a strip that is only gap would be all
    // handle).
    EXPECT(dragSpans(TabStrip{}, 8, 1.0).empty());
}

TEST_CASE("The frame is the system frame unless borderless works") {
    EXPECT(chooseFrame(false, false, "").mode == FrameMode::System);
    EXPECT(chooseFrame(false, false, "").note.empty());
    EXPECT(chooseFrame(false, true, "").mode == FrameMode::System);
    const FrameChoice own = chooseFrame(true, true, "");
    EXPECT(own.mode == FrameMode::Own);
    EXPECT(own.note.empty());
    const FrameChoice fell =
        chooseFrame(true, false, "that operation is not supported");
    EXPECT(fell.mode == FrameMode::System);
    EXPECT(fell.note.find("--borderless") != std::string::npos);
    EXPECT(fell.note.find("that operation is not supported") !=
           std::string::npos);
    EXPECT(fell.note.find("system frame") != std::string::npos);
    EXPECT(fell.note.back() != '\n');
}

TEST_SUIT_END
