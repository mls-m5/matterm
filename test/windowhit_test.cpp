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

TEST_CASE("A border is drawn round a normal window and not round a maximised one") {
    for (const int thickness : {1, 4}) {
        for (const auto &size : {std::pair{10, 7}, std::pair{1, 1}, std::pair{2, 2}, std::pair{3, 50}, std::pair{8, 8},
                                 std::pair{9, 9}, std::pair{1000, 600}}) {
            const auto ring = borderRects(size.first, size.second, false, thickness);
            // The rectangles cover every pixel of the outermost ring once and nothing inside.
            for (int y = 0; y < size.second; ++y) {
                for (int x = 0; x < size.first; ++x) {
                    int covered = 0;
                    for (const auto &r : ring) {
                        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) ++covered;
                    }
                    const bool edge = x < thickness || y < thickness || x >= size.first - thickness ||
                                      y >= size.second - thickness;
                    EXPECT_EQ(covered, (edge ? 1 : 0));
                }
            }
            for (const auto &r : ring) {
                EXPECT(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= size.first &&
                       r.y + r.h <= size.second);
            }
        }
    }
    EXPECT_EQ(borderRects(1000, 600, false).size(), std::size_t(4));
    EXPECT(borderRects(1000, 600, true).empty());
    EXPECT(borderRects(0, 0, false).empty());
    EXPECT(borderRects(-5, 10, false).empty());
    EXPECT(borderRects(100, 100, false, 0).empty());
    // A colour from the theme: between the background and the foreground, discreet, not either.
    const Palette palette;
    const Rgb8 c = borderColor(palette);
    const auto same = [](Rgb8 a, Rgb8 b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
    EXPECT(!same(c, palette.background) && !same(c, palette.foreground));
    EXPECT(c.r >= std::min(palette.background.r, palette.foreground.r) &&
           c.r <= std::max(palette.background.r, palette.foreground.r));
    // The outermost pixel is brighter than the band (when the text is brighter than the background).
    const Rgb8 edge = borderEdgeColor(palette);
    EXPECT_GT(int(edge.r) + int(edge.g) + int(edge.b) - (int(c.r) + int(c.g) + int(c.b)) +
                  (palette.foreground.r > palette.background.r ? 0 : 1000),
              0);
    // The border is part of the window: the edge pixel it covers is still a resize zone.
    WindowGeometry g = window();
    EXPECT(classifyWindowPoint(g, 0, 300) == Zone::ResizeLeft);
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
