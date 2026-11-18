#include "glyphatlas.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void check(int result) {
    if (result < 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

int run(const char *fontPath, bool smokeTest) {
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Terminal glyph atlas",
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
    const Uint32 flags =
        smokeTest ? SDL_RENDERER_SOFTWARE
                  : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), -1, flags), SDL_DestroyRenderer);
    if (!renderer) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RendererInfo info{};
    check(SDL_GetRendererInfo(renderer.get(), &info));
    std::cout << "SDL renderer: " << info.name << '\n';

    GlyphAtlas atlas(*renderer, fontPath, 24);
    const int cellWidth = std::max(1, atlas.getOrInsert(U'M').advanceX);
    const int cellHeight = atlas.lineHeight();
    const std::vector<std::u32string> lines{
        U"SDL2 glyph atlas - first rendering",
        U"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        U"abcdefghijklmnopqrstuvwxyz",
        U"0123456789 !@#$%^&*() []{} <> /\\",
        U"Baseline: Agjpqy  Accents: ÅÄÖ åäö éèü",
        U"Same atlas, different vertex colors.",
        U"Press Escape or close the window to exit."};
    // Resolve every glyph before submitting any geometry. No texture updates
    // occur while the draw batch is being assembled/submitted.
    for (const auto &line : lines) {
        for (char32_t ch : line) {
            atlas.getOrInsert(ch);
        }
    }
    int textureWidth, textureHeight;
    check(SDL_QueryTexture(
        atlas.texture(), nullptr, nullptr, &textureWidth, &textureHeight));
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    for (std::size_t row = 0; row < lines.size(); ++row) {
        const SDL_Color color = row % 2 ? SDL_Color{120, 220, 170, 255}
                                        : SDL_Color{220, 225, 245, 255};
        for (std::size_t column = 0; column < lines[row].size(); ++column) {
            const auto glyph = atlas.getOrInsert(lines[row][column]);
            if (!glyph.source.w || !glyph.source.h) {
                continue;
            }
            const float x =
                float(24 + int(column) * cellWidth + glyph.bearingX);
            const float y = float(24 + int(row) * cellHeight + atlas.ascent() -
                                  glyph.bearingY);
            const float right = x + glyph.source.w;
            const float bottom = y + glyph.source.h;
            const float u = float(glyph.source.x) / textureWidth;
            const float v = float(glyph.source.y) / textureHeight;
            const float u2 =
                float(glyph.source.x + glyph.source.w) / textureWidth;
            const float v2 =
                float(glyph.source.y + glyph.source.h) / textureHeight;
            const int base = int(vertices.size());
            vertices.insert(vertices.end(),
                            {{{x, y}, color, {u, v}},
                             {{right, y}, color, {u2, v}},
                             {{right, bottom}, color, {u2, v2}},
                             {{x, bottom}, color, {u, v2}}});
            for (int index : {0, 1, 2, 0, 2, 3}) {
                indices.push_back(base + index);
            }
        }
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            }
        }
        check(SDL_SetRenderDrawColor(renderer.get(), 14, 17, 22, 255));
        check(SDL_RenderClear(renderer.get()));
        check(SDL_RenderGeometry(renderer.get(),
                                 atlas.texture(),
                                 vertices.data(),
                                 int(vertices.size()),
                                 indices.data(),
                                 int(indices.size())));
        // A simple underline cursor; not yet connected to a terminal model.
        const SDL_Rect cursor{
            24 + 2 * cellWidth, 24 + 8 * cellHeight - 3, cellWidth, 2};
        check(SDL_SetRenderDrawColor(renderer.get(), 120, 220, 170, 255));
        check(SDL_RenderFillRect(renderer.get(), &cursor));
        if (smokeTest) {
            // Read only the text area, excluding the cursor. Verify that glyphs
            // actually reached the framebuffer, not merely that calls
            // succeeded.
            const SDL_Rect area{24, 24, 400, cellHeight};
            std::vector<Uint32> pixels(std::size_t(area.w) * area.h);
            check(SDL_RenderReadPixels(renderer.get(),
                                       &area,
                                       SDL_PIXELFORMAT_ARGB8888,
                                       pixels.data(),
                                       area.w * int(sizeof(Uint32))));
            const auto changed =
                std::count_if(pixels.begin(), pixels.end(), [](Uint32 pixel) {
                    return (pixel & 0xffffff) != 0x0e1116;
                });
            if (changed == 0) {
                throw std::runtime_error("Smoke test: no visible glyph pixels");
            }
            std::cout << "Glyph smoke test passed: " << changed
                      << " non-background pixels\n";
            running = false;
        }
        SDL_RenderPresent(renderer.get());
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
