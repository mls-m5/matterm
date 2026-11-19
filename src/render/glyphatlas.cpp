#include "glyphatlas.h"

#include <SDL_ttf.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;

void check(int result) {
    if (result < 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

Surface brailleBitmap(unsigned mask, int width, int height) {
    Surface pixels(SDL_CreateRGBSurfaceWithFormat(
                       0, width, height, 32, SDL_PIXELFORMAT_RGBA32),
                   SDL_FreeSurface);
    if (!pixels) {
        throw std::runtime_error(SDL_GetError());
    }
    check(SDL_FillRect(pixels.get(), nullptr, 0));
    const auto ink = SDL_MapRGBA(pixels->format, 255, 255, 255, 255);
    const int size = std::max(1, std::min(width / 4, height / 6));
    // Unicode dot numbers: 1–3 down the left, 4–6 down the right,
    // with dots 7 and 8 on the bottom row.
    constexpr unsigned dots[4][2] = {{1, 8}, {2, 16}, {4, 32}, {64, 128}};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 2; ++col) {
            if (mask & dots[row][col]) {
                const SDL_Rect dot{
                    std::max(0, ((2 * col + 1) * width / 4) - size / 2),
                    std::max(0, ((2 * row + 1) * height / 8) - size / 2),
                    size,
                    size};
                check(SDL_FillRect(pixels.get(), &dot, ink));
            }
        }
    }
    return pixels;
}
} // namespace

struct GlyphAtlas::Impl {
    std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font{nullptr,
                                                             TTF_CloseFont};
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture{
        nullptr, SDL_DestroyTexture};
    std::unordered_map<std::uint64_t, Glyph> glyphs;
    int width = 0;
    int height = 0;
    int x = 1;
    int y = 1;
    int shelfHeight = 0;
};

GlyphAtlas::GlyphAtlas(SDL_Renderer &renderer,
                       const std::string &fontPath,
                       int pixelSize,
                       int textureWidth,
                       int textureHeight)
    : impl_(std::make_unique<Impl>()) {
    if (pixelSize <= 0 || textureWidth <= 0 || textureHeight <= 0) {
        throw std::invalid_argument(
            "Atlas dimensions and font size must be positive");
    }
    // At 72 DPI, point size corresponds to pixel size.
    impl_->font.reset(TTF_OpenFontDPI(fontPath.c_str(), pixelSize, 72, 72));
    if (!impl_->font) {
        throw std::runtime_error(TTF_GetError());
    }
    impl_->width = textureWidth;
    impl_->height = textureHeight;
    impl_->texture.reset(SDL_CreateTexture(&renderer,
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,
                                           textureWidth,
                                           textureHeight));
    if (!impl_->texture) {
        throw std::runtime_error(SDL_GetError());
    }
    Surface blank(
        SDL_CreateRGBSurfaceWithFormat(
            0, textureWidth, textureHeight, 32, SDL_PIXELFORMAT_RGBA32),
        SDL_FreeSurface);
    if (!blank) {
        throw std::runtime_error(SDL_GetError());
    }
    check(SDL_FillRect(
        blank.get(), nullptr, SDL_MapRGBA(blank->format, 255, 255, 255, 0)));
    check(SDL_UpdateTexture(
        impl_->texture.get(), nullptr, blank->pixels, blank->pitch));
    check(SDL_SetTextureBlendMode(impl_->texture.get(), SDL_BLENDMODE_BLEND));
    check(SDL_SetTextureScaleMode(impl_->texture.get(), SDL_ScaleModeNearest));
}

GlyphAtlas::~GlyphAtlas() = default;

Glyph GlyphAtlas::getOrInsert(char32_t codePoint, CellAttribute attributes) {
    if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
        throw std::invalid_argument("Invalid Unicode scalar value");
    }
    auto &state = *impl_;
    const auto style =
        attributes & (CellAttribute::Bold | CellAttribute::Italic);
    const auto key = std::uint64_t(codePoint) | (std::uint64_t(style) << 32);
    if (const auto it = state.glyphs.find(key); it != state.glyphs.end()) {
        return it->second;
    }
    TTF_SetFontStyle(
        state.font.get(),
        (hasAttribute(style, CellAttribute::Bold) ? TTF_STYLE_BOLD : 0) |
            (hasAttribute(style, CellAttribute::Italic) ? TTF_STYLE_ITALIC
                                                        : 0));
    const bool available = TTF_GlyphIsProvided32(state.font.get(), codePoint);
    const bool drawBraille =
        !available && codePoint >= U'\u2800' && codePoint <= U'\u28ff';
    if (!available && !drawBraille) {
        for (const char32_t replacement : {U'\ufffd', U'?'}) {
            if (TTF_GlyphIsProvided32(state.font.get(), replacement)) {
                const auto glyph = getOrInsert(replacement, attributes);
                state.glyphs.emplace(key, glyph);
                return glyph;
            }
        }
        throw std::runtime_error("Font has no replacement glyph");
    }
    Glyph glyph;
    Surface pixels(nullptr, SDL_FreeSurface);
    if (drawBraille) {
        TTF_SetFontStyle(state.font.get(), TTF_STYLE_NORMAL);
        check(TTF_GlyphMetrics32(state.font.get(),
                                 U'M',
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 &glyph.advanceX));
        glyph.advanceX = std::max(1, glyph.advanceX);
        const unsigned mask = unsigned(codePoint - U'\u2800');
        if (!mask) {
            state.glyphs.emplace(key, glyph);
            return glyph;
        }
        const int height = std::max(1, lineHeight());
        glyph.bearingY = ascent();
        pixels = brailleBitmap(mask, glyph.advanceX, height);
    }
    else {
        int minX, maxX, minY, maxY;
        check(TTF_GlyphMetrics32(state.font.get(),
                                 codePoint,
                                 &minX,
                                 &maxX,
                                 &minY,
                                 &maxY,
                                 &glyph.advanceX));
        if (maxX <= minX || maxY <= minY) {
            state.glyphs.emplace(key, glyph);
            return glyph;
        }
        Surface rendered(
            TTF_RenderGlyph32_Blended(
                state.font.get(), codePoint, SDL_Color{255, 255, 255, 255}),
            SDL_FreeSurface);
        if (!rendered) {
            throw std::runtime_error(TTF_GetError());
        }
        pixels.reset(SDL_ConvertSurfaceFormat(
            rendered.get(), SDL_PIXELFORMAT_RGBA32, 0));
        if (!pixels) {
            throw std::runtime_error(SDL_GetError());
        }
        // SDL_ttf renders a baseline-aligned surface, including top/left
        // padding.
        glyph.bearingX = std::min(0, minX);
        glyph.bearingY = std::max(ascent(), maxY);
    }
    int x = state.x;
    int y = state.y;
    int shelfHeight = state.shelfHeight;
    if (pixels->w > state.width - 2 || pixels->h > state.height - 2) {
        throw std::runtime_error("Glyph is larger than the atlas");
    }
    if (x + pixels->w + 1 > state.width) {
        x = 1;
        y += shelfHeight + 1;
        shelfHeight = 0;
    }
    if (y + pixels->h + 1 > state.height) {
        throw std::runtime_error("Glyph atlas is full");
    }
    glyph.source = {x, y, pixels->w, pixels->h};
    check(SDL_UpdateTexture(
        state.texture.get(), &glyph.source, pixels->pixels, pixels->pitch));
    state.glyphs.emplace(key, glyph);
    state.x = x + pixels->w + 1;
    state.y = y;
    state.shelfHeight = std::max(shelfHeight, pixels->h);
    return glyph;
}

SDL_Texture *GlyphAtlas::texture() const noexcept {
    return impl_->texture.get();
}
int GlyphAtlas::ascent() const noexcept {
    return TTF_FontAscent(impl_->font.get());
}
int GlyphAtlas::lineHeight() const noexcept {
    return TTF_FontLineSkip(impl_->font.get());
}
