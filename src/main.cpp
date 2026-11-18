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
void appendDemoRow(TerminalMemory &memory, std::uint64_t number) {
    const auto label = std::to_string(number);
    std::u32string text = U"Row ";
    text.append(label.begin(), label.end());
    text += U" | block-ring scrollback | Agjpqy ÅÄÖ åäö";
    const Rgb8 foreground =
        number % 2 ? Rgb8{120, 220, 170} : Rgb8{220, 225, 245};
    memory.append(text, foreground, {14, 17, 22});
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
    TerminalMemory memory(std::size_t(backend.columns()), 256, 16);
    std::uint64_t nextRow = 1;
    // Start beyond capacity so wraparound and eviction are exercised
    // immediately.
    for (int i = 0; i < 300; ++i) {
        appendDemoRow(memory, nextRow++);
    }
    std::size_t scrollOffset =
        0; // Rows above the live bottom; zero follows output.
    bool paused = false;
    CursorState cursor;
    cursor.color = {120, 220, 170};
    Uint64 lastAppend = SDL_GetTicks64();
    bool running = true;
    while (running) {
        const auto columns = std::size_t(backend.columns());
        const auto rows = std::size_t(backend.rows());
        memory.resizeColumns(columns);
        const auto maxScroll = memory.size() - std::min(memory.size(), rows);
        scrollOffset = std::min(scrollOffset, maxScroll);
        const auto scrollUp = [&](std::size_t amount) {
            scrollOffset += std::min(amount, maxScroll - scrollOffset);
        };
        const auto scrollDown = [&](std::size_t amount) {
            scrollOffset -= std::min(amount, scrollOffset);
        };
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                std::int64_t amount = event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                if (amount > 0) {
                    scrollUp(std::size_t(amount) * 3);
                }
                else {
                    scrollDown(std::size_t(-amount) * 3);
                }
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_UP:
                    scrollUp(1);
                    break;
                case SDLK_DOWN:
                    scrollDown(1);
                    break;
                case SDLK_PAGEUP:
                    scrollUp(std::max<std::size_t>(1, rows));
                    break;
                case SDLK_PAGEDOWN:
                    scrollDown(std::max<std::size_t>(1, rows));
                    break;
                case SDLK_HOME:
                    scrollOffset = maxScroll;
                    break;
                case SDLK_END:
                    scrollOffset = 0;
                    break;
                case SDLK_SPACE:
                    paused = !paused;
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
        if (!paused && !smokeTest && now - lastAppend >= 100) {
            appendDemoRow(memory, nextRow++);
            // Preserve the viewed rows while reading history. Once those rows
            // are evicted, clamp to the oldest remaining page.
            if (scrollOffset != 0) {
                ++scrollOffset;
            }
            lastAppend = now;
        }
        const auto visibleCount = std::min(memory.size(), rows);
        scrollOffset = std::min(scrollOffset, memory.size() - visibleCount);
        const auto first = memory.size() - visibleCount - scrollOffset;
        cursor.column = std::min(4, std::max(0, int(columns) - 1));
        cursor.row = std::max(0, int(visibleCount) - 1);
        cursor.visible =
            scrollOffset == 0 && visibleCount != 0 && (now / 500) % 2 == 0;
        const auto title = std::string("Scrollback demo | wheel/PgUp/PgDn | "
                                       "Home/End | Space: pause | ") +
                           (paused ? "paused" : "streaming") + " | offset " +
                           std::to_string(scrollOffset) + " | retained " +
                           std::to_string(memory.size()) + "/" +
                           std::to_string(memory.capacity());
        SDL_SetWindowTitle(window.get(), title.c_str());
        const RenderFrame frame{memory.view(first, visibleCount), cursor};
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
