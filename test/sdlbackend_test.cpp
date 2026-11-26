#include "../src/frontend/splitdecoration.h"
#include "../src/frontend/themes.h"
#include "cellbuffer.h"
#include "mls-unit-test/unittest.h"
#include "sdlbackend.h"
#include "terminalmemory.h"
#include "terminalmodel.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
struct Libraries {
    Libraries() {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            throw std::runtime_error(SDL_GetError());
        }
        if (TTF_Init() < 0) {
            SDL_Quit();
            throw std::runtime_error(TTF_GetError());
        }
    }
    ~Libraries() {
        TTF_Quit();
        SDL_Quit();
    }
};

struct Fixture {
    Libraries libraries;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Backend test", 0, 0, 320, 160, SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow};
    std::unique_ptr<SdlBackend> backend;

    Fixture() {
        if (!window) {
            throw std::runtime_error(SDL_GetError());
        }
        const char *font = std::getenv("GLYPH_TEST_FONT");
        if (!font) {
            throw std::runtime_error("Set GLYPH_TEST_FONT");
        }
        backend = std::make_unique<SdlBackend>(*window, font, 24, true);
    }

    Uint32 pixel(int x, int y) {
        const SDL_Rect rect{x, y, 1, 1};
        Uint32 result;
        if (SDL_RenderReadPixels(SDL_GetRenderer(window.get()),
                                 &rect,
                                 SDL_PIXELFORMAT_ARGB8888,
                                 &result,
                                 sizeof(result)) < 0) {
            throw std::runtime_error(SDL_GetError());
        }
        return result & 0xffffff;
    }
};
} // namespace

TEST_SUIT_BEGIN(SdlBackend)

TEST_CASE("Themed split junctions survive pane rendering while external "
          "scrollbar stays visible") {
    using namespace frontend;
    Fixture fixture;
    const std::vector<DividerBox> dividers = {
        {{100, 0, 12, 160}, SplitDirection::SideBySide},
        {{112, 70, 100, 12}, SplitDirection::Stacked}};
    const auto decoration =
        splitDecoration(dividers, {0, 0, 320, 160}, true, false, false);
    CellBuffer cells(1, 1);
    RenderFrame frame;
    frame.cells = cells.view();
    frame.cursor.visible = false;
    frame.scrollbar = {true, 0.0f, 0.25f};
    frame.wrapMarks = {true};
    const auto packed = [](Rgb8 c) {
        return (Uint32(c.r) << 16) | (Uint32(c.g) << 8) | c.b;
    };
    for (const auto *theme : {"blue", "green"}) {
        const auto palette = themes::stripPalette(themes::theme(theme));
        fixture.backend->beginFrame(palette.background);
        paintSplitGutters(decoration, palette, [&](PixelRect r, Rgb8 c) {
            fixture.backend->drawRect({r.x, r.y, r.w, r.h}, c);
        });
        fixture.backend->render(frame, {{0, 0, 100, 160}, {100, 0, 6, 160}});
        EXPECT(fixture.pixel(109, 30) == packed(splitMiddleColor(palette)));
        EXPECT(fixture.pixel(111, 75) ==
               packed(splitMiddleColor(palette))); // T seam stays solid.
        EXPECT(fixture.pixel(120, 71) == packed(splitMiddleColor(palette)));
        EXPECT(fixture.pixel(120, 70) == packed(splitEdgeColor(palette)));
        EXPECT(fixture.pixel(102, 10) ==
               Uint32(0x6e7a8e)); // Existing thumb not painted over.
        EXPECT(fixture.pixel(107, fixture.backend->cellHeight() / 2) ==
               Uint32(0x6e7a8e)); // Wrap marker remains visible too.
        fixture.backend->present();
    }
}

TEST_CASE("Split decoration must not replace terminal right-strip background "
          "or scrollbar pixels") {
    using namespace frontend;
    Fixture fixture;
    fixture.backend->setReserveScrollbarStrip(true);
    std::size_t changedInterior = 0, changedRightmost = 0, changedGlyph = 0;
    for (bool own : {false, true})
        for (bool alternate : {false, true})
            for (bool history : {false, true})
                for (bool gutters : {false, true})
                    for (const auto *theme : {"grey", "blue"}) {
                        const auto palette =
                            themes::stripPalette(themes::theme(theme));
                        const auto inset = windowInset(own, false);
                        const int ox = inset.left,
                                  width = 320 - inset.left - inset.right,
                                  height = 160 - inset.bottom;
                        const int start = gutters ? 112 : 0;
                        TerminalModel model(
                            std::size_t((width - start - 12) /
                                        fixture.backend->cellWidth()),
                            4,
                            32);
                        if (history)
                            for (int i = 0; i < 16; ++i)
                                model.write(U"history\r\n");
                        if (alternate)
                            model.setAlternateScreen(true);
                        model.setColors({240, 240, 240}, {60, 90, 120});
                        model.eraseDisplay(TerminalModel::EraseMode::All);
                        model.moveCursor(0, 0);
                        model.write(U"\u2588");
                        auto frame = model.frame(fixture.backend->cellHeight());
                        frame.cursor.visible = false;
                        EXPECT(frame.scrollbar.visible ==
                               (history && !alternate));
                        frame.wrapMarks = {true, false, false, false};
                        const std::vector<DividerBox> dividers =
                            gutters
                                ? std::vector<
                                      DividerBox>{{{100, 20, 12, height - 20},
                                                   SplitDirection::SideBySide}}
                                : std::vector<DividerBox>{};
                        const auto renderPanes = [&] {
                            if (gutters)
                                fixture.backend->render(
                                    frame,
                                    {{ox, 20, 100, height - 20},
                                     {ox + 100, 20, 6, height - 20}});
                            fixture.backend->render(
                                frame,
                                {{ox + start, 20, width - start, height - 20},
                                 {}});
                        };
                        fixture.backend->beginFrame(palette.background);
                        fixture.backend->drawRect({ox, 0, width, 20},
                                                  splitMiddleColor(palette));
                        renderPanes();
                        for (const auto &d : dividers)
                            fixture.backend->drawRect(
                                {ox + d.rect.x, d.rect.y, d.rect.w, d.rect.h},
                                {46, 51, 60});
                        std::vector<Uint32> before;
                        const auto glyphBefore = fixture.pixel(
                            ox + start, 20 + fixture.backend->cellHeight() / 2);
                        for (int x = width - 12; x < width; ++x)
                            before.push_back(fixture.pixel(
                                ox + x,
                                20 + fixture.backend->cellHeight() + 5));
                        fixture.backend->beginFrame(palette.background);
                        fixture.backend->drawRect({ox, 0, width, 20},
                                                  splitMiddleColor(palette));
                        const auto plan =
                            splitDecoration(dividers,
                                            {0, 20, width, height - 20},
                                            own,
                                            false,
                                            false);
                        const auto paint = [&](PixelRect r, Rgb8 c) {
                            fixture.backend->drawRect({r.x + ox, r.y, r.w, r.h},
                                                      c);
                        };
                        paintSplitGutters(plan, palette, paint);
                        renderPanes();
                        for (const auto &r : plan.outer)
                            paint(r, splitEdgeColor(palette));
                        if (own) {
                            for (const auto &r : borderRects(320, 160, false))
                                fixture.backend->drawRect(
                                    {r.x, r.y, r.w, r.h},
                                    splitMiddleColor(palette));
                            const std::vector<PixelRect> scrollbars =
                                frame.scrollbar.visible
                                    ? std::vector<PixelRect>{{width - 12,
                                                              20,
                                                              12,
                                                              height - 20}}
                                    : std::vector<PixelRect>{};
                            for (const auto &r :
                                 ownFrameDarkEdges({0, 20, width, height - 20},
                                                   dividers,
                                                   scrollbars,
                                                   true,
                                                   false,
                                                   false))
                                paint(r, splitEdgeColor(palette));
                            const auto color = [](Rgb8 c) {
                                return (Uint32(c.r) << 16) |
                                       (Uint32(c.g) << 8) | c.b;
                            };
                            EXPECT_EQ(fixture.pixel(ox - 1, 40),
                                      color(splitEdgeColor(palette)));
                            EXPECT_EQ(fixture.pixel(ox - 2, 40),
                                      color(splitMiddleColor(palette)));
                            EXPECT_EQ(fixture.pixel(ox + width, 40),
                                      color(frame.scrollbar.visible
                                                ? splitMiddleColor(palette)
                                                : splitEdgeColor(palette)));
                            if (gutters)
                                EXPECT_EQ(fixture.pixel(ox + 109, 19),
                                          color(splitMiddleColor(palette)));
                        }
                        EXPECT_EQ(fixture.pixel(
                                      ox + width - 10,
                                      20 + fixture.backend->cellHeight() / 2),
                                  Uint32(0x6e7a8e));
                        if (frame.scrollbar.visible)
                            EXPECT_EQ(fixture.pixel(ox + width - 3, height - 3),
                                      Uint32(0x6e7a8e));
                        if (gutters) {
                            EXPECT_EQ(
                                fixture.pixel(
                                    ox + 107,
                                    20 + fixture.backend->cellHeight() / 2),
                                Uint32(0x6e7a8e));
                            if (frame.scrollbar.visible)
                                EXPECT_EQ(fixture.pixel(ox + 102, height - 3),
                                          Uint32(0x6e7a8e));
                        }
                        for (int x = width - 12; x < width; ++x) {
                            if (fixture.pixel(
                                    ox + x,
                                    20 + fixture.backend->cellHeight() + 5) !=
                                before[std::size_t(x - width + 12)]) {
                                if (x == width - 1)
                                    ++changedRightmost;
                                else
                                    ++changedInterior;
                            }
                        }
                        if (fixture.pixel(ox + start,
                                          20 + fixture.backend->cellHeight() /
                                                   2) != glyphBefore)
                            ++changedGlyph;
                        fixture.backend->present();
                    }
    EXPECT_EQ(changedInterior, std::size_t(0));
    EXPECT_EQ(changedRightmost, std::size_t(0));
    EXPECT_EQ(changedGlyph, std::size_t(0));
}

TEST_CASE("Backend renders independent rows and per-cell backgrounds") {
    Fixture fixture;
    const FString first{FChar{U' ', {}, {200, 30, 40}},
                        FChar{U' ', {}, {20, 180, 50}}};
    const FString second{FChar{U' ', {}, {10, 20, 210}},
                         FChar{U' ', {}, {90, 80, 70}}};
    const FStringView rows[]{FStringView(first), FStringView(second)};
    RenderBackend &backend = *fixture.backend;
    backend.render({CellGridView(rows, 2, 2), {}});
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    EXPECT_EQ(fixture.pixel(1, 1), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(w + 1, 1), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(1, h + 1), Uint32(0x0a14d2));
    EXPECT_EQ(fixture.pixel(w + 1, h + 1), Uint32(0x5a5046));
    EXPECT_EQ(fixture.pixel(w * 3, 1), Uint32(0x1e1e1e));
    backend.present();
}

TEST_CASE("A viewport confines sizes and drawing to one pane") {
    Fixture fixture; // The window is 320x160.
    const Uint32 gap = 0x0a141e;
    fixture.backend->clear({10, 20, 30});
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    CellBuffer cells(4, 2);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    fixture.backend->setViewport(100, 20, 96, 64);
    EXPECT_EQ(fixture.backend->columns(), 96 / w);
    EXPECT_EQ(fixture.backend->rows(), 64 / h);
    RenderFrame frame{cells.view(), {}};
    fixture.backend->render(frame);
    // Coordinates are relative to the pane; nothing leaks outside it.
    EXPECT_EQ(fixture.pixel(101, 21), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(99, 21), gap);
    EXPECT_EQ(fixture.pixel(101, 19), gap);
    EXPECT_EQ(fixture.pixel(196, 21), gap);
    EXPECT_EQ(fixture.pixel(101, 84), gap);
    EXPECT_EQ(fixture.pixel(0, 0), gap);
    // The rest of the pane gets the palette background, not the window's.
    EXPECT_EQ(fixture.pixel(100 + 95, 20 + 63), Uint32(0x1e1e1e));
    // A scrollbar sits at the pane's right edge.
    frame.scrollbar = {true, 0.0f, 1.0f};
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(100 + 85, 30), Uint32(0x1e1e1e));
    EXPECT_EQ(fixture.pixel(100 + 90, 30), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(100 + 96, 30), gap);
    // Another pane next to it, without disturbing the first.
    fixture.backend->setViewport(200, 20, 96, 64);
    CellBuffer other(4, 2);
    other.clear(FChar{U' ', {}, {20, 180, 50}});
    fixture.backend->render({other.view(), {}});
    EXPECT_EQ(fixture.pixel(201, 21), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(101, 21), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(198, 21), gap);
    // Back to the whole drawable.
    fixture.backend->resetViewport();
    EXPECT_EQ(fixture.backend->columns(), 320 / w);
    EXPECT_EQ(fixture.backend->rows(), 160 / h);
    fixture.backend->render({cells.view(), {}});
    EXPECT_EQ(fixture.pixel(1, 1), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(300, 150), Uint32(0x1e1e1e));
}

TEST_CASE("A viewport must be a non-empty rectangle") {
    Fixture fixture;
    for (const auto &rect : std::vector<std::array<int, 4>>{{0, 0, 0, 10},
                                                            {0, 0, 10, 0},
                                                            {0, 0, -5, 10},
                                                            {-1, 0, 10, 10},
                                                            {0, -1, 10, 10}}) {
        bool threw = false;
        try {
            fixture.backend->setViewport(rect[0], rect[1], rect[2], rect[3]);
        }
        catch (const std::invalid_argument &) {
            threw = true;
        }
        EXPECT(threw);
    }
    // A refused viewport leaves the sizes alone.
    EXPECT_EQ(fixture.backend->columns(), 320 / fixture.backend->cellWidth());
}

TEST_CASE("Backend renders a history view across ring and block boundaries") {
    Fixture fixture;
    TerminalMemory memory(1, 3, 2);
    memory.append(U" ", {}, {10, 0, 0});
    memory.append(U" ", {}, {20, 0, 0});
    memory.append(U" ", {}, {30, 0, 0});
    memory.append(U" ", {}, {40, 0, 0});
    fixture.backend->render({memory.view(0, 3), {}});
    const int h = fixture.backend->cellHeight();
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x140000));
    EXPECT_EQ(fixture.pixel(0, h), Uint32(0x1e0000));
    EXPECT_EQ(fixture.pixel(0, h * 2), Uint32(0x280000));
    fixture.backend->render({memory.view(1, 2), {}});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x1e0000));
    EXPECT_EQ(fixture.pixel(0, h), Uint32(0x280000));
    EXPECT_EQ(fixture.pixel(0, h * 2), Uint32(0x1e1e1e));
}

TEST_CASE("Fractional rows are clipped to text bounds and cannot cover the "
          "scrollbar") {
    Fixture fixture;
    CellBuffer cells(40, 4);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    for (std::size_t x = 0; x < cells.columns(); ++x) {
        cells.at(x, 1).background = {20, 180, 50};
    }
    const int h = fixture.backend->cellHeight();
    const int shift = h / 2;
    RenderFrame frame{cells.view(), {}};
    frame.verticalOffsetPixels = -float(shift);
    frame.textHeightPixels = h;
    frame.scrollbar = {true, 1.0f, 0.25f};
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(0, h - shift), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(0, h + 1), Uint32(0x1e1e1e));
    EXPECT_EQ(fixture.pixel(314, 1), Uint32(0x1e1e1e));
    EXPECT_EQ(fixture.pixel(314, 130), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(319, 130), Uint32(0x1e1e1e));
    frame.scrollbar.position = 0;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(314, 1), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(314, 130), Uint32(0x1e1e1e));
    frame.scrollbar.size = 1;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(314, 130), Uint32(0x6e7a8e));
    frame.textHeightPixels = 0;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x1e1e1e));
    frame.textHeightPixels = h;
    frame.scrollbar.visible = false;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(319, 0), Uint32(0xc81e28));
}

TEST_CASE("Wrapped rows get a mark in the strip; no cell changes, unwrapped "
          "rows get none") {
    Fixture fixture;
    fixture.backend->setReserveScrollbarStrip(true);
    const auto columns = std::size_t(fixture.backend->columns(true));
    const int h = fixture.backend->cellHeight();
    const int width = 320; // The fixture's window.
    CellBuffer cells(columns, 4);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    RenderFrame frame{cells.view(), {}};
    frame.wrapMarks = {true, false, true, false};
    fixture.backend->render(frame);
    const int strip = width - 12;
    // Row 0 and 2 continue: the middle of the mark's base column is the dim
    // foreground.
    const int markX = strip + 2;
    EXPECT_EQ(fixture.pixel(markX, h / 2), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(markX, 2 * h + h / 2), Uint32(0x6e7a8e));
    // Row 1 and 3 end there: the strip looks like the cell beside it.
    EXPECT_EQ(fixture.pixel(markX, h + h / 2), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(markX, 3 * h + h / 2), Uint32(0xc81e28));
    // Above and below the mark, still the cell colour; the cells themselves are
    // untouched.
    EXPECT_EQ(fixture.pixel(markX, 1), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(markX, h - 1), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(1, h / 2), Uint32(0xc81e28));
    EXPECT_EQ(
        fixture.pixel(int(columns) * fixture.backend->cellWidth() - 1, h / 2),
        Uint32(0xc81e28));
    // No marks asked for (empty): nothing drawn there.
    frame.wrapMarks.clear();
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(markX, h / 2), Uint32(0xc81e28));
}

TEST_CASE("Marks and the scrollbar share the strip: the thumb moves right, the "
          "mark stays visible") {
    Fixture fixture;
    fixture.backend->setReserveScrollbarStrip(true);
    const auto columns = std::size_t(fixture.backend->columns(true));
    const int h = fixture.backend->cellHeight();
    CellBuffer cells(columns, 4);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    RenderFrame frame{cells.view(), {}};
    frame.scrollbar = {true, 0.0f, 1.0f};
    frame.wrapMarks = {true, false, false, false};
    fixture.backend->render(frame);
    const int strip = 320 - 12;
    EXPECT_EQ(fixture.pixel(strip + 2, h / 2), Uint32(0x6e7a8e)); // the mark
    EXPECT_EQ(fixture.pixel(strip + 2, 2 * h),
              Uint32(0xc81e28)); // the neighbouring cell background
    EXPECT_EQ(fixture.pixel(strip + 9, 2 * h),
              Uint32(0x6e7a8e)); // the thumb, narrower and to the right
    // Without marks the thumb stays the same narrow width.
    frame.wrapMarks.clear();
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(strip + 2, 2 * h), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(strip + 7, 2 * h), Uint32(0x6e7a8e));
}

TEST_CASE("A pane beside a divider: the scrollbar in the near half, the mark "
          "in the far half") {
    Fixture fixture; // 320x160
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    const Uint32 gap = 0x0a141e;
    fixture.backend->beginFrame({10, 20, 30});
    const int paneWidth = (96 / w) * w;
    CellBuffer cells(std::size_t(96 / w), 3);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    RenderFrame frame{cells.view(), {}};
    frame.scrollbar = {true, 0.0f, 0.25f};
    frame.wrapMarks = {true, false, false};
    // The pane at x=20, its divider 12 px wide right of it; the scrollbar is
    // the near half (6 px).
    const int x = 20, y = 10;
    fixture.backend->render(
        frame,
        PaneArea{{x, y, paneWidth, 3 * h}, {x + paneWidth, y, 6, 3 * h}});
    // Near half: the scrollbar as before, no mark there.
    EXPECT_EQ(fixture.pixel(x + paneWidth, y + h / 2), gap);
    // Far half: the mark on the wrapped row's middle, nothing on the next row,
    // the gap elsewhere.
    EXPECT_EQ(fixture.pixel(x + paneWidth + 6 + 1, y + h / 2),
              Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(x + paneWidth + 6 + 1, y + h + h / 2), gap);
    // Nothing leaks past the divider.
    EXPECT_EQ(fixture.pixel(x + paneWidth + 12 + 2, y + h / 2), gap);
    // A scrollbar half too narrow for the mark: not drawn.
    fixture.backend->beginFrame({10, 20, 30});
    fixture.backend->render(
        frame,
        PaneArea{{x, y, paneWidth, 3 * h}, {x + paneWidth, y, 4, 3 * h}});
    EXPECT_EQ(fixture.pixel(x + paneWidth + 4 + 1, y + h / 2), gap);
}

TEST_CASE("Without a reserved strip there is no room, so no mark") {
    Fixture fixture;
    const int h = fixture.backend->cellHeight();
    CellBuffer cells(8, 2);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    RenderFrame frame{cells.view(), {}};
    frame.wrapMarks = {true, false};
    fixture.backend->render(frame);
    for (int x = 0; x < 320; x += 7) {
        const auto p = fixture.pixel(x, h / 2);
        EXPECT(p == Uint32(0xc81e28) || p == Uint32(0x1e1e1e));
    }
}

TEST_CASE(
    "A reserved strip without a scrollbar continues the last cell per row") {
    Fixture fixture;
    // The frontend's column count leaves the 12 px strip (and any remainder)
    // to the right of the last column.
    const auto columns = std::size_t(fixture.backend->columns(true));
    CellBuffer cells(columns, 4);
    cells.clear(FChar{U' ', {}, {200, 30, 40}});
    for (std::size_t y = 0; y < cells.rows(); ++y) {
        cells.at(columns - 1, y).background =
            y == 1 ? Rgb8{20, 180, 50} : Rgb8{200, 30, 40};
    }
    RenderFrame frame{cells.view(), {}};
    const int h = fixture.backend->cellHeight();
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(319, 0), Uint32(0x1e1e1e)); // Default: untouched.
    fixture.backend->setReserveScrollbarStrip(true);
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(319, 0), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(319, h + 1), Uint32(0x14b432));
    frame.scrollbar = {true, 0.0f, 0.5f};
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(319, 0),
              Uint32(0xc81e28)); // Cell background continues.
}

TEST_CASE(
    "Pixel offset also moves the cursor and invalid offsets fail cleanly") {
    Fixture fixture;
    CellBuffer cells(2, 3);
    RenderFrame frame{cells.view(), {}};
    frame.cursor.visible = true;
    frame.cursor.row = 1;
    frame.cursor.shape = CursorState::Shape::Bar;
    frame.cursor.color = {255, 0, 0};
    const int h = fixture.backend->cellHeight();
    frame.verticalOffsetPixels = -float(h / 2);
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, h - h / 2), Uint32(0xff0000));
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0));
    frame.verticalOffsetPixels = -float(h);
    EXPECT_THROW(fixture.backend->render(frame), std::invalid_argument);
    frame.verticalOffsetPixels = 1;
    EXPECT_THROW(fixture.backend->render(frame), std::invalid_argument);
    fixture.backend->render({});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x1e1e1e));
}

TEST_CASE("A focused block cursor is opaque and inverts the glyph under it") {
    Fixture fixture;
    CellBuffer cells(1, 1);
    cells.draw(0, 0, U"M", {0, 255, 0}, {50, 0, 0});
    CursorState cursor;
    cursor.visible = true;
    cursor.shape = CursorState::Shape::Block;
    cursor.color = {255, 255, 255};
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0xffffff));
    EXPECT_EQ(fixture.pixel(w - 1, h - 1), Uint32(0xffffff));
    bool inverted = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            inverted |= fixture.pixel(x, y) == Uint32(0x320000);
        }
    }
    EXPECT(inverted); // The glyph is drawn in the cell's own background.
    EXPECT_EQ(cells.at(0, 0).background.r, 50);
}

TEST_CASE("A gliding block cursor is translucent at a fractional position") {
    Fixture fixture;
    CellBuffer cells(3, 1);
    cells.draw(0, 0, U"M", {0, 255, 0}, {0, 0, 0});
    CursorState cursor;
    cursor.visible = true;
    cursor.shape = CursorState::Shape::Block;
    cursor.color = {255, 255, 255};
    cursor.column = 1;
    cursor.columnOffset = -0.5f; // Shown half a cell left of column 1.
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    fixture.backend->render({cells.view(), cursor});
    // Inside the moving block (half over cell 0, half over cell 1), not the
    // opaque white of a settled block, and the settled cell is not inverted.
    const auto inside = fixture.pixel(w, h - 1);
    EXPECT(inside != Uint32(0xffffff));
    EXPECT(inside != Uint32(0x000000));
    EXPECT_EQ(fixture.pixel(w * 2 + w / 2, h - 1), Uint32(0));
}

TEST_CASE("An unfocused cursor is a hollow outline in every shape") {
    Fixture fixture;
    CellBuffer cells(1, 1);
    CursorState cursor;
    cursor.visible = true;
    cursor.focused = false;
    cursor.shape = CursorState::Shape::Block;
    cursor.color = {255, 0, 0};
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0xff0000));
    EXPECT_EQ(fixture.pixel(w - 1, h / 2), Uint32(0xff0000));
    EXPECT_EQ(fixture.pixel(w / 2, h - 1), Uint32(0xff0000));
    EXPECT_EQ(fixture.pixel(w / 2, h / 2), Uint32(0));
    cursor.shape = CursorState::Shape::Bar;
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(w / 2, h / 2), Uint32(0));
}

TEST_CASE("Glyph geometry uses the cell foreground color") {
    Fixture fixture;
    CellBuffer cells(1, 1);
    cells.draw(0, 0, U"M", {0, 255, 0}, {50, 0, 0});
    fixture.backend->render({cells.view(), {}});
    bool greenGlyph = false;
    for (int y = 0; y < fixture.backend->cellHeight(); ++y) {
        for (int x = 0; x < fixture.backend->cellWidth(); ++x) {
            const auto pixel = fixture.pixel(x, y);
            greenGlyph |= ((pixel >> 8) & 255) > ((pixel >> 16) & 255);
        }
    }
    EXPECT(greenGlyph);
}

TEST_CASE(
    "Cursor shapes overlay cells without modifying them or leaving trails") {
    Fixture fixture;
    CellBuffer cells(2, 1);
    CursorState cursor;
    cursor.visible = true;
    cursor.color = {255, 0, 0};
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(0, h - 1), Uint32(0xff0000));
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0));
    cursor.column = 1;
    cursor.shape = CursorState::Shape::Bar;
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(0, h - 1), Uint32(0));
    EXPECT_EQ(fixture.pixel(w, 0), Uint32(0xff0000));
    cursor.shape = CursorState::Shape::Block;
    fixture.backend->render({cells.view(), cursor});
    EXPECT_NE(fixture.pixel(w + w / 2, h / 2), Uint32(0));
    EXPECT(cells.at(1, 0).codePoint == U' ');
    EXPECT_EQ(cells.at(1, 0).background.r, 0);
    cursor.visible = false;
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(w, 0), Uint32(0));
    cursor.visible = true;
    cursor.column = -1;
    fixture.backend->render({cells.view(), cursor});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0));
}

TEST_CASE(
    "Edited cells appear on the next frame and empty frames clear the output") {
    Fixture fixture;
    {
        CellBuffer cells(1, 1);
        fixture.backend->render({cells.view(), {}});
        cells.at(0, 0).background = {10, 20, 30};
        fixture.backend->render({cells.view(), {}});
        EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x0a141e));
    }
    fixture.backend->present(); // No borrowed cell storage is needed here.
    fixture.backend->render({});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x1e1e1e));
    EXPECT_GT(fixture.backend->columns(), 0);
    EXPECT_GT(fixture.backend->rows(), 0);
}

TEST_CASE("Underline strike and faint render even on space cells") {
    Fixture fixture;
    CellBuffer cells(2, 1);
    cells.at(0, 0) = FChar{U' ',
                           {20, 200, 40},
                           {0, 0, 0},
                           CellAttribute::Underline | CellAttribute::Faint};
    cells.at(1, 0) =
        FChar{U' ', {200, 40, 20}, {0, 0, 0}, CellAttribute::Strike};
    fixture.backend->render({cells.view(), {}});
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    EXPECT_EQ(fixture.pixel(1, h - 2), Uint32(0x0a6414));
    EXPECT_EQ(fixture.pixel(w + 1, h / 2), Uint32(0xc82814));
    EXPECT_EQ(fixture.pixel(1, 0), Uint32(0));
}

TEST_CASE(
    "Text blink hides ink and decorations but preserves cell backgrounds") {
    Fixture fixture;
    CellBuffer cells(1, 1);
    cells.at(0, 0) = FChar{U'M',
                           {20, 200, 40},
                           {10, 20, 30},
                           CellAttribute::Blink | CellAttribute::Underline};
    RenderFrame frame{cells.view(), {}};
    frame.textBlinkVisible = false;
    fixture.backend->render(frame);
    for (int y = 0; y < fixture.backend->cellHeight(); ++y) {
        for (int x = 0; x < fixture.backend->cellWidth(); ++x) {
            EXPECT_EQ(fixture.pixel(x, y), Uint32(0x0a141e));
        }
    }
    frame.textBlinkVisible = true;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(1, fixture.backend->cellHeight() - 2),
              Uint32(0x14c828));
}

TEST_CASE("A new palette recolors existing frame cells without touching their "
          "storage") {
    Fixture fixture;
    CellBuffer cells(2, 1);
    cells.at(0, 0).setBackground(ColorReference::indexed(1));
    cells.at(1, 0).setBackground(ColorReference::defaultColor());
    Palette palette;
    palette.colors[1] = {10, 20, 30};
    palette.background = {40, 50, 60};
    RenderFrame frame{cells.view(), {}};
    frame.palette = &palette;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x0a141e));
    EXPECT_EQ(fixture.pixel(fixture.backend->cellWidth(), 0), Uint32(0x28323c));
    palette.colors[1] = {70, 80, 90};
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x46505a));
    EXPECT_EQ(cells.at(0, 0).backgroundColor().value.r, 1);
}

TEST_CASE("Presentation underline spans wrapped rows and clears without "
          "modifying cells") {
    Fixture fixture;
    CellBuffer cells(3, 2);
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 0; x < 3; ++x) {
            cells.at(x, y) = FChar{U' ', {20, 200, 40}, {10, 20, 30}};
        }
    }
    cells.at(1, 0).attributes = CellAttribute::Blink;
    RenderFrame frame{cells.view(), {}};
    frame.textBlinkVisible = false;
    frame.underlineBegin = 1;
    frame.underlineEnd = 5;
    frame.selectionBegin = 2;
    frame.selectionEnd = 3;
    fixture.backend->render(frame);
    const int w = fixture.backend->cellWidth(),
              h = fixture.backend->cellHeight();
    EXPECT_EQ(fixture.pixel(1, h - 2), Uint32(0x0a141e));
    EXPECT_EQ(fixture.pixel(w + 1, h - 2), Uint32(0x14c828));
    EXPECT_EQ(fixture.pixel(2 * w + 1, h - 2), Uint32(0xffffff));
    EXPECT_EQ(fixture.pixel(1, 2 * h - 2), Uint32(0x14c828));
    EXPECT_EQ(fixture.pixel(2 * w + 1, 2 * h - 2), Uint32(0x0a141e));
    EXPECT(cells.at(1, 0).attributes == CellAttribute::Blink);
    frame.underlineBegin = frame.underlineEnd = 0;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(w + 1, h - 2), Uint32(0x0a141e));
    EXPECT_EQ(fixture.pixel(1, 2 * h - 2), Uint32(0x0a141e));
}

TEST_CASE(
    "Presentation underline follows fractional scroll and text clipping") {
    Fixture fixture;
    CellBuffer cells(1, 2);
    for (std::size_t y = 0; y < 2; ++y) {
        cells.at(0, y) = FChar{U' ', {20, 200, 40}, {10, 20, 30}};
    }
    const int h = fixture.backend->cellHeight();
    RenderFrame frame{cells.view(), {}};
    frame.underlineEnd = 2;
    frame.verticalOffsetPixels = -float(h / 2);
    frame.textHeightPixels = h;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(1, h - 2 - h / 2), Uint32(0x14c828));
    EXPECT_EQ(fixture.pixel(1, 2 * h - 2 - h / 2), Uint32(0x1e1e1e));
}

TEST_CASE("A frame of several panes: clear once, leftovers like the neighbour "
          "cell, dividers, scrollbars in the gutter") {
    Fixture fixture;
    const int w = fixture.backend->cellWidth();
    const int h = fixture.backend->cellHeight();
    const Uint32 gap = 0x010203;
    fixture.backend->beginFrame({1, 2, 3});
    EXPECT_EQ(fixture.pixel(0, 0), gap);
    EXPECT_EQ(fixture.pixel(319, 159), gap);
    // Pane A: 2x1 cells in a rectangle with 5 pixels to the right and 7 below.
    CellBuffer a(2, 1);
    a.at(0, 0) = FChar{U' ', {}, {200, 30, 40}};
    a.at(1, 0) = FChar{U' ', {}, {20, 180, 50}};
    RenderFrame paneFrame{a.view(), {}};
    paneFrame.textHeightPixels =
        h; // A terminal model clips at its full-cell height.
    fixture.backend->render(paneFrame, PaneArea{{0, 0, 2 * w + 5, h + 7}, {}});
    EXPECT_EQ(fixture.pixel(1, 1), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(w + 1, 1), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(2 * w + 2, 1), Uint32(0x14b432)); // Right.
    EXPECT_EQ(fixture.pixel(1, h + 3), Uint32(0xc81e28));     // Below.
    EXPECT_EQ(fixture.pixel(w + 1, h + 3), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(2 * w + 2, h + 3), Uint32(0x14b432)); // Corner.
    EXPECT_EQ(fixture.pixel(2 * w + 6, 1), gap); // Not beyond the pane.
    EXPECT_EQ(fixture.pixel(1, h + 8), gap);
    // Bottom alignment shifts the cells, but paints the small gap above them
    // from the first row and keeps the last row flush with the pane's bottom.
    fixture.backend->render(paneFrame,
                            PaneArea{{0, 0, 2 * w + 5, h + 7}, {}, true});
    EXPECT_EQ(fixture.pixel(1, 0), Uint32(0xc81e28));
    EXPECT_EQ(fixture.pixel(w + 1, 0), Uint32(0x14b432));
    EXPECT_EQ(fixture.pixel(w + 1, h + 6), Uint32(0x14b432));
    paneFrame.cursor.visible = true;
    paneFrame.cursor.focused = false;
    paneFrame.cursor.color = {255, 255, 255};
    fixture.backend->render(paneFrame,
                            PaneArea{{0, 0, 2 * w + 5, h + 7}, {}, true});
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0xc81e28)); // No cursor in the inset.
    EXPECT_EQ(fixture.pixel(0, 7),
              Uint32(0xffffff)); // The cursor moved with the cells.
    // Pane B elsewhere does not disturb A (no second clear).
    CellBuffer b(2, 1);
    b.clear(FChar{U' ', {}, {10, 20, 210}});
    fixture.backend->render(RenderFrame{b.view(), {}},
                            PaneArea{{200, 0, 2 * w, h}, {}});
    EXPECT_EQ(fixture.pixel(201, 1), Uint32(0x0a14d2));
    EXPECT_EQ(fixture.pixel(1, 1), Uint32(0xc81e28));
    // A divider is a filled strip over the gap.
    fixture.backend->drawRect({100, 0, 10, 160}, {9, 9, 9});
    EXPECT_EQ(fixture.pixel(105, 5), Uint32(0x090909));
    EXPECT_EQ(fixture.pixel(99, 5), gap);
    EXPECT_EQ(fixture.pixel(110, 5), gap);
    // A pane's scrollbar sits in the gutter and takes no columns from the text.
    CellBuffer c(2, 1);
    c.clear(FChar{U' ', {}, {90, 80, 70}});
    RenderFrame withBar{c.view(), {}};
    withBar.scrollbar = {true, 0.0f, 1.0f};
    fixture.backend->render(withBar,
                            PaneArea{{0, 100, 2 * w, h}, {2 * w, 100, 4, h}});
    EXPECT_EQ(fixture.pixel(2 * w - 1, 101), Uint32(0x5a5046)); // Text intact.
    EXPECT_EQ(fixture.pixel(2 * w, 101), gap); // Divider background.
    EXPECT_EQ(fixture.pixel(2 * w + 1, 105), Uint32(0x6e7a8e)); // Thumb.
    EXPECT_EQ(fixture.pixel(2 * w + 4, 101), gap);
    // Without a gutter strip the legacy strip inside the pane is used.
    fixture.backend->render(withBar, PaneArea{{150, 100, 2 * w, h}, {}});
    EXPECT_EQ(fixture.pixel(150 + 2 * w - 6, 105), Uint32(0x6e7a8e));
}

TEST_CASE("The atlas generation grows at each present, and a bad pane area "
          "throws") {
    Fixture fixture;
    const auto first = fixture.backend->atlasGeneration();
    fixture.backend->present();
    fixture.backend->present();
    EXPECT_EQ(fixture.backend->atlasGeneration(), first + 2);
    fixture.backend->setPixelSize(20);
    EXPECT_EQ(fixture.backend->atlasGeneration(), first + 2); // Kept counting.
    CellBuffer cells(1, 1);
    for (const auto &area : {PaneArea{{0, 0, 0, 10}, {}},
                             PaneArea{{-1, 0, 10, 10}, {}},
                             PaneArea{{0, 0, 10, 10}, {-2, 0, 4, 10}}}) {
        bool threw = false;
        try {
            fixture.backend->render(RenderFrame{cells.view(), {}}, area);
        }
        catch (const std::invalid_argument &) {
            threw = true;
        }
        EXPECT(threw);
    }
    // Still usable, with the whole drawable.
    fixture.backend->render(RenderFrame{cells.view(), {}});
}

TEST_SUIT_END
