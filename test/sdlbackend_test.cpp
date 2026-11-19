#include "cellbuffer.h"
#include "mls-unit-test/unittest.h"
#include "sdlbackend.h"
#include "terminalmemory.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>

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
    EXPECT_EQ(fixture.pixel(312, 1), Uint32(0x191d24));
    EXPECT_EQ(fixture.pixel(312, 130), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(319, 130), Uint32(0x191d24));
    frame.scrollbar.position = 0;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(312, 1), Uint32(0x6e7a8e));
    EXPECT_EQ(fixture.pixel(312, 130), Uint32(0x191d24));
    frame.scrollbar.size = 1;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(312, 130), Uint32(0x6e7a8e));
    frame.textHeightPixels = 0;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(0, 0), Uint32(0x1e1e1e));
    frame.textHeightPixels = h;
    frame.scrollbar.visible = false;
    fixture.backend->render(frame);
    EXPECT_EQ(fixture.pixel(319, 0), Uint32(0xc81e28));
}

TEST_CASE("A reserved strip without a scrollbar continues the last cell per row") {
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
    EXPECT_EQ(fixture.pixel(319, 0), Uint32(0x191d24)); // The track wins.
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

TEST_SUIT_END
