#include "democontent.h"
#include "scrollviewport.h"
#include "sdlbackend.h"
#include "terminalmemory.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
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
        SDL_CreateWindow("matterm",
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
    TerminalMemory memory(std::size_t(backend.columns(true)), 256, 16);
    std::uint64_t nextRow = 1;
    demo::welcome(memory, std::size_t(backend.rows()));
    ScrollViewport viewport;
    viewport.setGeometry(memory.size(), std::size_t(backend.rows()));
    viewport.oldest();
    viewport.advance(
        10); // Show the banner immediately, including in small windows.
    bool streaming = false;
    bool welcomeScreen = true;
    CursorState cursor;
    cursor.color = {120, 220, 170};
    Uint64 lastAppend = SDL_GetTicks64();
    Uint64 lastFrame = lastAppend;
    bool running = true;
    while (running) {
        const auto columns = std::size_t(backend.columns(true));
        const auto rows = std::size_t(backend.rows());
        if (welcomeScreen &&
            (memory.columns() != columns ||
             memory.size() != std::min(memory.capacity(),
                                       std::max<std::size_t>(20, rows)))) {
            memory.resizeColumns(columns);
            demo::welcome(memory, rows);
        }
        else {
            memory.resizeColumns(columns);
        }
        viewport.setGeometry(memory.size(), rows);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                viewport.scrollBy(amount * 3.0);
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_UP:
                    viewport.scrollBy(1);
                    break;
                case SDLK_DOWN:
                    viewport.scrollBy(-1);
                    break;
                case SDLK_PAGEUP:
                    viewport.scrollBy(double(std::max<std::size_t>(1, rows)));
                    break;
                case SDLK_PAGEDOWN:
                    viewport.scrollBy(-double(std::max<std::size_t>(1, rows)));
                    break;
                case SDLK_HOME:
                    viewport.oldest();
                    break;
                case SDLK_END:
                    viewport.followTail();
                    break;
                case SDLK_s:
                case SDLK_SPACE:
                    if (!event.key.repeat) {
                        streaming = !streaming;
                        welcomeScreen = false;
                        lastAppend = SDL_GetTicks64();
                        if (streaming) {
                            viewport.followTail();
                        }
                    }
                    break;
                case SDLK_b:
                    if (!event.key.repeat) {
                        welcomeScreen = false;
                        for (int i = 0; i < 300; ++i) {
                            demo::appendNumberedRow(memory, nextRow++);
                        }
                        viewport.rowsAppended(300, memory.size());
                    }
                    break;
                case SDLK_r:
                    if (!event.key.repeat) {
                        streaming = false;
                        welcomeScreen = true;
                        nextRow = 1;
                        demo::welcome(memory, rows);
                        viewport.setGeometry(memory.size(), rows);
                        viewport.oldest();
                        viewport.advance(10);
                    }
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
        const Uint64 now = SDL_GetTicks64();
        if (streaming && !smokeTest && now - lastAppend >= 100) {
            demo::appendNumberedRow(memory, nextRow++);
            viewport.rowsAppended(1, memory.size());
            lastAppend = now;
        }
        viewport.advance(double(now - lastFrame) / 1000.0);
        lastFrame = now;
        cursor.column = std::min(4, std::max(0, int(columns) - 1));
        cursor.row = std::max(0, int(viewport.rowCount()) - 1);
        cursor.visible = viewport.atTail() && viewport.rowCount() != 0 &&
                         rows != 0 && (now / 500) % 2 == 0;
        const auto title =
            std::string("matterm | S: stream | B: burst | R: welcome | ") +
            (streaming ? "streaming" : "idle") + " | offset " +
            std::to_string(viewport.offset()) + " | retained " +
            std::to_string(memory.size()) + "/" +
            std::to_string(memory.capacity());
        SDL_SetWindowTitle(window.get(), title.c_str());
        RenderFrame frame{memory.view(viewport.firstRow(), viewport.rowCount()),
                          cursor};
        frame.verticalOffsetPixels = viewport.pixelOffset(backend.cellHeight());
        frame.textHeightPixels = int(rows) * backend.cellHeight();
        frame.scrollbar = viewport.scrollbar();
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
    demo::printBanner(std::cout);
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
