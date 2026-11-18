#pragma once

#include "glyph.h"

#include <memory>
#include <string>

// SDL2/SDL2_ttf backend resource.
// Owns one font at one pixel size and one SDL texture. Use only on the
// renderer's thread, and destroy the atlas before destroying the renderer.
class GlyphAtlas {
public:
    // Opens the font and creates a fixed-size atlas. The SDL renderer is
    // borrowed. Requires SDL_ttf to be initialized and SDL2_ttf >= 2.0.18 for
    // 32-bit APIs. Failure is reported by exception. All dimensions must be
    // positive.
    GlyphAtlas(SDL_Renderer &renderer,
               const std::string &fontPath,
               int pixelSize,
               int textureWidth = 1024,
               int textureHeight = 1024);
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas &) = delete;
    GlyphAtlas &operator=(const GlyphAtlas &) = delete;
    GlyphAtlas(GlyphAtlas &&) = delete;
    GlyphAtlas &operator=(GlyphAtlas &&) = delete;

    // Returns cached metrics or rasterizes and uploads a white coverage glyph.
    // Foreground color is applied by the draw code, not baked into the cache.
    // Cache key: Unicode scalar value; the font and size are fixed for this
    // atlas. Rejects invalid scalars; missing characters use a cached
    // replacement glyph. Throws on rasterization/upload failure or when the
    // fixed atlas is full. Collect glyphs before submitting geometry to avoid
    // mid-batch uploads.
    Glyph getOrInsert(char32_t codePoint);

    // Borrowed texture; callers must not destroy it or modify its contents.
    // Use alpha blending and vertex colors when drawing atlas geometry.
    SDL_Texture *texture() const noexcept;

    // Font-wide layout metrics. Glyph bearingY is relative to this baseline.
    int ascent() const noexcept;
    int lineHeight() const noexcept;

private:
    // Font, texture, packing state, and cache stay out of the public header.
    // The destructor must be defined in the implementation where Impl is
    // complete.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
