#include "glyphatlas.h"

#include <SDL_ttf.h>
#ifdef MATTERM_HAS_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <map>
#endif

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
using Font = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;

#ifdef MATTERM_HAS_FONTCONFIG
class SystemFonts {
public:
    TTF_Font *match(char32_t codePoint, int pixelSize) {
        if (!FcInit()) {
            return nullptr;
        }
        const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern{
            FcPatternCreate(), FcPatternDestroy};
        const std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)> charset{
            FcCharSetCreate(), FcCharSetDestroy};
        if (!pattern || !charset ||
            !FcCharSetAddChar(charset.get(), codePoint) ||
            !FcPatternAddCharSet(pattern.get(), FC_CHARSET, charset.get()) ||
            !FcPatternAddString(
                pattern.get(),
                FC_FAMILY,
                reinterpret_cast<const FcChar8 *>("monospace")) ||
            !FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern)) {
            return nullptr;
        }
        FcDefaultSubstitute(pattern.get());
        FcResult result;
        const std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> matches{
            FcFontSort(nullptr, pattern.get(), FcFalse, nullptr, &result),
            FcFontSetDestroy};
        for (int i = 0; matches && i < matches->nfont; ++i) {
            auto *candidate = matches->fonts[i];
            FcCharSet *coverage = nullptr;
            FcChar8 *file = nullptr;
            int index = 0;
            if (FcPatternGetCharSet(candidate, FC_CHARSET, 0, &coverage) !=
                    FcResultMatch ||
                !FcCharSetHasChar(coverage, codePoint) ||
                FcPatternGetString(candidate, FC_FILE, 0, &file) !=
                    FcResultMatch) {
                continue;
            }
            FcPatternGetInteger(candidate, FC_INDEX, 0, &index);
            const auto *path = reinterpret_cast<const char *>(file);
            auto [it, inserted] =
                faces.try_emplace({path, index}, nullptr, TTF_CloseFont);
            if (inserted) {
                it->second.reset(
                    TTF_OpenFontIndexDPI(path, pixelSize, index, 72, 72));
            }
            if (auto *font = it->second.get();
                font && TTF_GlyphIsProvided32(font, codePoint)) {
                return font;
            }
        }
        return nullptr;
    }

private:
    std::map<std::pair<std::string, int>, Font> faces;
};
#endif

void check(int result) {
    if (result < 0) {
        throw std::runtime_error(SDL_GetError());
    }
}
} // namespace

struct GlyphAtlas::Impl {
    Font font{nullptr, TTF_CloseFont};
#ifdef MATTERM_HAS_FONTCONFIG
    SystemFonts systemFonts;
    int pixelSize = 0;
#endif
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
#ifdef MATTERM_HAS_FONTCONFIG
    impl_->pixelSize = pixelSize;
#endif
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
    auto *font = state.font.get();
    TTF_Font *fallback = nullptr;
    const bool provided = TTF_GlyphIsProvided32(font, codePoint);
#ifdef MATTERM_HAS_FONTCONFIG
    if (!provided) {
        fallback = state.systemFonts.match(codePoint, state.pixelSize);
    }
#endif
    if (fallback) {
        font = fallback;
    }
    else if (!provided) {
        for (const char32_t replacement : {U'\ufffd', U'?'}) {
            if (TTF_GlyphIsProvided32(state.font.get(), replacement)) {
                const auto glyph = getOrInsert(replacement, attributes);
                state.glyphs.emplace(key, glyph);
                return glyph;
            }
        }
        throw std::runtime_error("Font has no replacement glyph");
    }
    const int cellAdvance =
        fallback && codePoint != U'M' ? getOrInsert(U'M').advanceX : 0;
    TTF_SetFontStyle(
        font,
        (hasAttribute(style, CellAttribute::Bold) ? TTF_STYLE_BOLD : 0) |
            (hasAttribute(style, CellAttribute::Italic) ? TTF_STYLE_ITALIC
                                                        : 0));
    int minX, maxX, minY, maxY, advance;
    check(TTF_GlyphMetrics32(
        font, codePoint, &minX, &maxX, &minY, &maxY, &advance));
    Glyph glyph;
    glyph.advanceX = cellAdvance > 0 ? cellAdvance : advance;
    if (maxX <= minX || maxY <= minY) {
        state.glyphs.emplace(key, glyph);
        return glyph;
    }
    Surface rendered(TTF_RenderGlyph32_Blended(
                         font, codePoint, SDL_Color{255, 255, 255, 255}),
                     SDL_FreeSurface);
    if (!rendered) {
        throw std::runtime_error(TTF_GetError());
    }
    Surface pixels(
        SDL_ConvertSurfaceFormat(rendered.get(), SDL_PIXELFORMAT_RGBA32, 0),
        SDL_FreeSurface);
    if (!pixels) {
        throw std::runtime_error(SDL_GetError());
    }
    // SDL_ttf renders a baseline-aligned surface, including its top/left
    // padding.
    glyph.bearingX = std::min(0, minX);
    glyph.bearingY = std::max(TTF_FontAscent(font), maxY);
    if (fallback) {
        // Keep the primary font's grid. Shrink and center wider fallback
        // bitmaps rather than letting them overlap neighboring cells.
        const int cellWidth = std::max(1, glyph.advanceX);
        const int cellHeight = std::max(1, lineHeight());
        if (pixels->w > cellWidth || pixels->h > cellHeight) {
            const double scale = std::min(double(cellWidth) / pixels->w,
                                          double(cellHeight) / pixels->h);
            const int w = std::max(1, int(pixels->w * scale));
            const int h = std::max(1, int(pixels->h * scale));
            Surface fitted(SDL_CreateRGBSurfaceWithFormat(
                               0, w, h, 32, SDL_PIXELFORMAT_RGBA32),
                           SDL_FreeSurface);
            if (!fitted) {
                throw std::runtime_error(SDL_GetError());
            }
            check(SDL_SetSurfaceBlendMode(pixels.get(), SDL_BLENDMODE_NONE));
            check(SDL_BlitScaled(pixels.get(), nullptr, fitted.get(), nullptr));
            glyph.bearingY = int(glyph.bearingY * scale);
            pixels = std::move(fitted);
        }
        glyph.bearingX = (cellWidth - pixels->w) / 2;
        glyph.bearingY = std::clamp(
            glyph.bearingY, ascent() - (cellHeight - pixels->h), ascent());
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
