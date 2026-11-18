#include "sdlbackend.h"

#include "glyphatlas.h"

#include <SDL.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void check(int result) {
    if (result < 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

SDL_Renderer *createRenderer(SDL_Window &window, bool software) {
    auto *renderer = SDL_CreateRenderer(
        &window,
        -1,
        software ? SDL_RENDERER_SOFTWARE
                 : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        throw std::runtime_error(SDL_GetError());
    }
    return renderer;
}

struct Batch {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    void clear() {
        vertices.clear();
        indices.clear();
    }

    void quad(float x,
              float y,
              float w,
              float h,
              SDL_Color color,
              float u = 0,
              float v = 0,
              float u2 = 0,
              float v2 = 0) {
        if (vertices.size() >
                std::size_t(std::numeric_limits<int>::max() - 4) ||
            indices.size() > std::size_t(std::numeric_limits<int>::max() - 6)) {
            throw std::length_error("SDL geometry batch too large");
        }
        const int base = int(vertices.size());
        vertices.insert(vertices.end(),
                        {{{x, y}, color, {u, v}},
                         {{x + w, y}, color, {u2, v}},
                         {{x + w, y + h}, color, {u2, v2}},
                         {{x, y + h}, color, {u, v2}}});
        for (int i : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(base + i);
        }
    }

    void draw(SDL_Renderer &renderer, SDL_Texture *texture) const {
        if (!indices.empty()) {
            check(SDL_RenderGeometry(&renderer,
                                     texture,
                                     vertices.data(),
                                     int(vertices.size()),
                                     indices.data(),
                                     int(indices.size())));
        }
    }
};
} // namespace

struct SdlBackend::Impl {
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer;
    GlyphAtlas atlas;
    SDL_RendererInfo info{};
    int cellWidth;
    int cellHeight;
    int textureWidth;
    int textureHeight;
    Batch backgrounds;
    Batch glyphs;

    Impl(SDL_Window &window,
         const std::string &fontPath,
         int size,
         bool software)
        : renderer(createRenderer(window, software), SDL_DestroyRenderer)
        , atlas(*renderer, fontPath, size)
        , cellWidth(std::max(1, atlas.getOrInsert(U'M').advanceX))
        , cellHeight(std::max(1, atlas.lineHeight())) {
        check(SDL_GetRendererInfo(renderer.get(), &info));
        check(SDL_QueryTexture(
            atlas.texture(), nullptr, nullptr, &textureWidth, &textureHeight));
    }
};

SdlBackend::SdlBackend(SDL_Window &window,
                       const std::string &fontPath,
                       int pixelSize,
                       bool software)
    : impl_(std::make_unique<Impl>(window, fontPath, pixelSize, software)) {}

SdlBackend::~SdlBackend() = default;

void SdlBackend::render(const RenderFrame &frame) {
    auto &s = *impl_;
    int width, height;
    check(SDL_GetRendererOutputSize(s.renderer.get(), &width, &height));
    const auto columns =
        std::min(frame.cells.columns(),
                 std::size_t(width / s.cellWidth + (width % s.cellWidth != 0)));
    const auto rows = std::min(
        frame.cells.rows(),
        std::size_t(height / s.cellHeight + (height % s.cellHeight != 0)));
    s.backgrounds.clear();
    s.glyphs.clear();

    // Resolve all cache misses before submitting draw calls. The frame's views
    // are consumed synchronously; only copied vertex data survives this call.
    for (std::size_t y = 0; y < rows; ++y) {
        const auto row = frame.cells.row(y);
        for (std::size_t x = 0; x < columns; ++x) {
            s.atlas.getOrInsert(row.data()[x].codePoint);
        }
    }
    for (std::size_t y = 0; y < rows; ++y) {
        const auto row = frame.cells.row(y);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto &cell = row.data()[x];
            const float left = float(x) * s.cellWidth;
            const float top = float(y) * s.cellHeight;
            s.backgrounds.quad(
                left,
                top,
                float(s.cellWidth),
                float(s.cellHeight),
                {cell.background.r, cell.background.g, cell.background.b, 255});
            const auto glyph = s.atlas.getOrInsert(cell.codePoint);
            if (glyph.source.w == 0 || glyph.source.h == 0) {
                continue;
            }
            s.glyphs.quad(
                left + glyph.bearingX,
                top + s.atlas.ascent() - glyph.bearingY,
                float(glyph.source.w),
                float(glyph.source.h),
                {cell.foreground.r, cell.foreground.g, cell.foreground.b, 255},
                float(glyph.source.x) / s.textureWidth,
                float(glyph.source.y) / s.textureHeight,
                float(glyph.source.x + glyph.source.w) / s.textureWidth,
                float(glyph.source.y + glyph.source.h) / s.textureHeight);
        }
    }
    check(SDL_SetRenderDrawColor(s.renderer.get(), 14, 17, 22, 255));
    check(SDL_RenderClear(s.renderer.get()));
    s.backgrounds.draw(*s.renderer, nullptr);
    s.glyphs.draw(*s.renderer, s.atlas.texture());

    const auto &cursor = frame.cursor;
    if (cursor.visible && cursor.column >= 0 && cursor.row >= 0 &&
        std::size_t(cursor.column) < columns &&
        std::size_t(cursor.row) < rows) {
        SDL_FRect rect{float(cursor.column) * s.cellWidth,
                       float(cursor.row) * s.cellHeight,
                       float(s.cellWidth),
                       float(s.cellHeight)};
        Uint8 alpha = 255;
        if (cursor.shape == CursorState::Shape::Underline) {
            rect.h = float(std::min(2, s.cellHeight));
            rect.y += s.cellHeight - rect.h;
        }
        else if (cursor.shape == CursorState::Shape::Bar) {
            rect.w = float(std::min(2, s.cellWidth));
        }
        else {
            alpha = 128; // A translucent overlay keeps the underlying glyph
                         // visible.
        }
        check(
            SDL_SetRenderDrawBlendMode(s.renderer.get(), SDL_BLENDMODE_BLEND));
        check(SDL_SetRenderDrawColor(s.renderer.get(),
                                     cursor.color.r,
                                     cursor.color.g,
                                     cursor.color.b,
                                     alpha));
        check(SDL_RenderFillRectF(s.renderer.get(), &rect));
        check(SDL_SetRenderDrawBlendMode(s.renderer.get(), SDL_BLENDMODE_NONE));
    }
}

void SdlBackend::present() {
    SDL_RenderPresent(impl_->renderer.get());
}
int SdlBackend::cellWidth() const noexcept {
    return impl_->cellWidth;
}
int SdlBackend::cellHeight() const noexcept {
    return impl_->cellHeight;
}
int SdlBackend::columns() const {
    int width, height;
    check(SDL_GetRendererOutputSize(impl_->renderer.get(), &width, &height));
    return width / cellWidth();
}
int SdlBackend::rows() const {
    int width, height;
    check(SDL_GetRendererOutputSize(impl_->renderer.get(), &width, &height));
    return height / cellHeight();
}
const char *SdlBackend::rendererName() const noexcept {
    return impl_->info.name;
}
