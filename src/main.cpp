#include "cellbuffer.h"
#include "sdlbackend.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void paintDemo(CellBuffer &buffer) {
    const Rgb8 background{14, 17, 22};
    buffer.clear(FChar{U' ', {220, 225, 245}, background});
    buffer.draw(1,
                0,
                U"CellBuffer -> RenderFrame -> SdlBackend",
                {120, 220, 170},
                background);
    buffer.draw(1,
                2,
                U"A temporary cell-grid demo, not a terminal yet.",
                {220, 225, 245},
                background);
    buffer.draw(1,
                4,
                U"ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789",
                {240, 200, 110},
                background);
    buffer.draw(1,
                5,
                U"Baseline: Agjpqy  Accents: ÅÄÖ åäö éèü",
                {160, 180, 250},
                background);
    buffer.draw(1,
                7,
                U"Each cell has its own foreground and background.",
                {220, 225, 245},
                background);
    for (int x = 1; x < 40; ++x) {
        buffer.draw(x,
                    9,
                    U" ",
                    {255, 255, 255},
                    {Uint8(40 + x * 4), Uint8(40 + x * 2), Uint8(160 - x * 2)});
    }
    buffer.draw(1,
                11,
                U"Arrows: move cursor   Tab: cursor shape",
                {120, 220, 170},
                background);
    buffer.draw(1,
                12,
                U"Resize: redraw grid   Escape: exit",
                {120, 220, 170},
                background);
    buffer.draw(1,
                14,
                U"The marker blinks without changing stored cells.",
                {180, 185, 200},
                background);
}

void verifyGlyphPixels(SDL_Window &window, const SdlBackend &backend) {
    int width, height;
    SDL_Renderer *renderer = SDL_GetRenderer(&window);
    if (SDL_GetRendererOutputSize(renderer, &width, &height) < 0) {
        throw std::runtime_error(SDL_GetError());
    }
    const SDL_Rect area{
        0, 0, std::min(400, width), std::min(backend.cellHeight(), height)};
    std::vector<Uint32> pixels(std::size_t(area.w) * area.h);
    if (SDL_RenderReadPixels(renderer,
                             &area,
                             SDL_PIXELFORMAT_ARGB8888,
                             pixels.data(),
                             area.w * int(sizeof(Uint32))) < 0) {
        throw std::runtime_error(SDL_GetError());
    }
    const auto changed =
        std::count_if(pixels.begin(), pixels.end(), [](Uint32 pixel) {
            return (pixel & 0xffffff) != 0x0e1116;
        });
    if (changed == 0) {
        throw std::runtime_error("Smoke test: no visible glyph pixels");
    }
    std::cout << "Cell rendering smoke test passed: " << changed
              << " glyph pixels\n";
}

int run(const char *fontPath, bool smokeTest) {
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Terminal cell grid",
                         SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED,
                         960,
                         640,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                             (smokeTest ? SDL_WINDOW_HIDDEN : 0)),
        SDL_DestroyWindow);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SdlBackend backend(*window, fontPath, 24, smokeTest);
    std::cout << "SDL renderer: " << backend.rendererName() << '\n';
    CellBuffer buffer(std::size_t(backend.columns()),
                      std::size_t(backend.rows()));
    paintDemo(buffer);
    CursorState cursor;
    cursor.column = 2;
    cursor.row = 16;
    cursor.color = {120, 220, 170};
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_LEFT:
                    --cursor.column;
                    break;
                case SDLK_RIGHT:
                    ++cursor.column;
                    break;
                case SDLK_UP:
                    --cursor.row;
                    break;
                case SDLK_DOWN:
                    ++cursor.row;
                    break;
                case SDLK_TAB:
                    cursor.shape = cursor.shape == CursorState::Shape::Underline
                                       ? CursorState::Shape::Bar
                                   : cursor.shape == CursorState::Shape::Bar
                                       ? CursorState::Shape::Block
                                       : CursorState::Shape::Underline;
                    break;
                default:
                    break;
                }
            }
        }
        if (!running) {
            break;
        }
        const auto columns = std::size_t(backend.columns());
        const auto rows = std::size_t(backend.rows());
        if (columns != buffer.columns() || rows != buffer.rows()) {
            buffer.resize(columns, rows);
            paintDemo(buffer);
        }
        cursor.column =
            std::clamp(cursor.column, 0, std::max(0, int(columns) - 1));
        cursor.row = std::clamp(cursor.row, 0, std::max(0, int(rows) - 1));
        cursor.visible = smokeTest || (SDL_GetTicks64() / 500) % 2 == 0;
        const RenderFrame frame{buffer.view(), cursor};
        backend.render(frame);
        if (smokeTest) {
            verifyGlyphPixels(*window, backend);
            running = false;
        }
        backend.present();
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::string_view(argv[2]) != "--smoke-test")) {
        std::cerr << "Usage: " << argv[0]
                  << " /path/to/monospace.ttf [--smoke-test]\n";
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cerr << TTF_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    int result = 1;
    try {
        result = run(argv[1], argc == 3);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    TTF_Quit();
    SDL_Quit();
    return result;
}
