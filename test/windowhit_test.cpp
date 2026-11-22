#include "mls-unit-test/unittest.h"
#include "probelayout.h"
#include "tabstrip.h"
#include "windowhit.h"

#include <string>
#include <utility>
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

TEST_CASE("The borders are as wide as decided: 4 px visible, 8 px grip, 16 px corners") {
    EXPECT_EQ(kBorderWidth, 4);
    EXPECT_EQ(kResizeEdge, 8);
    EXPECT_EQ(kResizeCorner, 16);
    EXPECT_GT(kResizeEdge, kBorderWidth); // The grip is wider than what is drawn, never narrower.
}

TEST_CASE("The border is on the left, right and bottom only, and never over the view") {
    for (const auto &size : {std::pair{10, 7}, std::pair{1, 1}, std::pair{2, 2}, std::pair{3, 50}, std::pair{8, 8},
                             std::pair{9, 9}, std::pair{1000, 600}}) {
        const int w = size.first, h = size.second;
        const auto ring = borderRects(w, h, false);
        const WindowInset inset = windowInset(true, false);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int covered = 0;
                for (const auto &r : ring) {
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) ++covered;
                }
                const bool edge = x < kBorderWidth || x >= w - kBorderWidth || y >= h - kBorderWidth;
                EXPECT_EQ(covered, (edge ? 1 : 0)); // Every border pixel once, nothing else, no top edge.
                // The view is what the border leaves: the same pixels, not one more or less.
                const bool inView = x >= inset.left && x < w - inset.right && y >= inset.top && y < h - inset.bottom;
                EXPECT_EQ(inView, !edge);
            }
        }
        for (const auto &r : ring) {
            EXPECT(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h);
        }
    }
    EXPECT_EQ(borderRects(1000, 600, false).size(), std::size_t(3));
    EXPECT(borderRects(1000, 600, true).empty());
    EXPECT(borderRects(0, 0, false).empty());
    EXPECT(borderRects(-5, 10, false).empty());
    EXPECT(borderRects(100, 100, false, 0).empty());
    // The top of the window is the strip: no inset there, ever.
    EXPECT_EQ(windowInset(true, false).top, 0);
    EXPECT_EQ(windowInset(true, false).left, kBorderWidth);
    EXPECT_EQ(windowInset(true, false).right, kBorderWidth);
    EXPECT_EQ(windowInset(true, false).bottom, kBorderWidth);
    // No inset with the system frame, and none when maximised.
    const WindowInset none = windowInset(false, false);
    EXPECT(none.left == 0 && none.right == 0 && none.top == 0 && none.bottom == 0);
    const WindowInset maximised = windowInset(true, true);
    EXPECT(maximised.left == 0 && maximised.right == 0 && maximised.top == 0 && maximised.bottom == 0);
}

TEST_CASE("The border has exactly the colour of the strip where the window is dragged") {
    const Palette palette;
    const Rgb8 border = borderColor(palette);
    std::vector<TabInfo> tabs(1);
    tabs[0].title = "a";
    tabs[0].active = true;
    const TabStrip strip = buildTabStrip(tabs, 80, Palette(), 0, true, ShareButton{true, 0}, WindowButtons{true, false});
    const auto spans = dragSpans(strip, 8, 1.0);
    EXPECT(!spans.empty());
    for (const auto &span : spans) {
        for (int px = span.begin; px < span.end; px += 8) {
            const Rgb8 bg = strip.cells[std::size_t(px / 8)].background;
            EXPECT(bg.r == border.r && bg.g == border.g && bg.b == border.b);
        }
    }
}

TEST_CASE("The grip is the 4 px of border and 4 px of content: the border is not the whole grip") {
    EXPECT_EQ(kResizeEdge, kBorderWidth + 4);
    WindowGeometry g = window();
    const WindowInset inset = windowInset(true, false);
    // The first content pixel (just inside the border) still resizes.
    EXPECT(classifyWindowPoint(g, inset.left, 300) == Zone::ResizeLeft);
    EXPECT(classifyWindowPoint(g, kResizeEdge, 300) == Zone::Normal);
}

TEST_CASE("The pointer shows the resize shape the zone asks for") {
    EXPECT(cursorShapeFor(Zone::Normal) == CursorShape::Default);
    EXPECT(cursorShapeFor(Zone::Drag) == CursorShape::Default);
    EXPECT(cursorShapeFor(Zone::ResizeLeft) == CursorShape::ResizeEastWest);
    EXPECT(cursorShapeFor(Zone::ResizeRight) == CursorShape::ResizeEastWest);
    EXPECT(cursorShapeFor(Zone::ResizeTop) == CursorShape::ResizeNorthSouth);
    EXPECT(cursorShapeFor(Zone::ResizeBottom) == CursorShape::ResizeNorthSouth);
    EXPECT(cursorShapeFor(Zone::ResizeTopLeft) == CursorShape::ResizeNorthwestSoutheast);
    EXPECT(cursorShapeFor(Zone::ResizeBottomRight) == CursorShape::ResizeNorthwestSoutheast);
    EXPECT(cursorShapeFor(Zone::ResizeTopRight) == CursorShape::ResizeNortheastSouthwest);
    EXPECT(cursorShapeFor(Zone::ResizeBottomLeft) == CursorShape::ResizeNortheastSouthwest);
    // The same function the hit test uses: every resize zone of a real window gives a resize shape,
    // the handle and the inside give the default; a maximised window has no resize shape anywhere.
    WindowGeometry g = window();
    for (int y = 0; y < g.height; y += 3) {
        for (int x = 0; x < g.width; x += 3) {
            const Zone z = classifyWindowPoint(g, x, y);
            EXPECT((cursorShapeFor(z) != CursorShape::Default) == (z != Zone::Normal && z != Zone::Drag));
        }
    }
    g.maximized = true;
    for (int y = 0; y < g.height; y += 7) {
        for (int x = 0; x < g.width; x += 7) {
            EXPECT(cursorShapeFor(classifyWindowPoint(g, x, y)) == CursorShape::Default);
        }
    }
}

TEST_CASE("The drag map follows the view when it is moved right by the border") {
    std::vector<TabInfo> tabs(1);
    tabs[0].title = "a";
    const TabStrip strip = buildTabStrip(tabs, 80, Palette(), 0, true, ShareButton{true, 0}, WindowButtons{true, false});
    const auto plain = dragSpans(strip, 8, 1.0);
    const auto moved = dragSpans(strip, 8, 1.0, kBorderWidth);
    EXPECT_EQ(plain.size(), moved.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(moved[i].begin, plain[i].begin + kBorderWidth);
        EXPECT_EQ(moved[i].end, plain[i].end + kBorderWidth);
    }
    // High DPI: the offset is in drawable pixels like the cells.
    const auto half = dragSpans(strip, 8, 2.0, 8);
    EXPECT_EQ(half.front().begin, (plain.front().begin + 8) / 2);
}

TEST_CASE("Every pixel of the edge band outside the strip handle resizes, whatever the size") {
    for (const auto &size : {std::pair{200, 100}, std::pair{1000, 600}, std::pair{1919, 1079}, std::pair{40, 40}}) {
        WindowGeometry g;
        g.width = size.first;
        g.height = size.second;
        g.stripHeight = 16;
        g.drag = {{50, g.width - 50}};
        for (int y = 0; y < g.height; ++y) {
            for (int x = 0; x < g.width; ++x) {
                const bool band = x < kResizeEdge || y < kResizeEdge || x >= g.width - kResizeEdge ||
                                  y >= g.height - kResizeEdge;
                const WindowZone z = classifyWindowPoint(g, x, y);
                const bool resize = z != Zone::Normal && z != Zone::Drag;
                EXPECT_EQ(resize, band);
            }
        }
    }
}

TEST_CASE("A window call is only FAILED when it returned an error; a state that is not reported is its own word") {
    EXPECT(std::string(callVerdict(true, true)) == "ok");
    EXPECT(std::string(callVerdict(true, false)) == "called, state not reported");
    EXPECT(std::string(callVerdict(false, true)) == "FAILED");
    EXPECT(std::string(callVerdict(false, false)) == "FAILED");
}

TEST_CASE("The transparency probe picture nests: transparent rim, red margin, solid box") {
    const ProbeLayout layout = probeLayout(360, 240, 40);
    EXPECT(!layout.red.empty() && !layout.box.empty());
    EXPECT_EQ(layout.red.x, 20); // A 20 px rim outside the red stays transparent.
    EXPECT_EQ(layout.box.x, 40);
    EXPECT(layout.red.x + layout.red.w <= 360 && layout.red.y + layout.red.h <= 240);
    EXPECT(layout.box.x >= layout.red.x && layout.box.y >= layout.red.y);
    EXPECT(layout.box.x + layout.box.w <= layout.red.x + layout.red.w);
    EXPECT(layout.box.y + layout.box.h <= layout.red.y + layout.red.h);
    EXPECT(probeLayout(60, 60, 40).box.empty()); // Too small: nothing to show.
    EXPECT(probeLayout(360, 240, 0).red.empty());
    EXPECT(probeLayout(-1, -1, 10).box.empty());
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
