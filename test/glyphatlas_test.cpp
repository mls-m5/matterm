#include "glyphatlas.h"
#include "mls-unit-test/unittest.h"

#include <SDL_ttf.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace {
struct Libraries {
    Libraries() {
        if (SDL_Init(0) < 0) {
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
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface{
        SDL_CreateRGBSurfaceWithFormat(0, 128, 128, 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
        surface ? SDL_CreateSoftwareRenderer(surface.get()) : nullptr, SDL_DestroyRenderer};

    Fixture() {
        if (!renderer) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    const char *font() const {
        const char *path = std::getenv("GLYPH_TEST_FONT");
        if (!path || !*path) {
            throw std::runtime_error("Set GLYPH_TEST_FONT to a TrueType font file");
        }
        return path;
    }
};
} // namespace

TEST_SUIT_BEGIN(GlyphAtlas)

TEST_CASE("Glyphs are cached with stable rectangles and metrics") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24, 256, 256);
    const auto first = atlas.getOrInsert(U'A');
    atlas.getOrInsert(U'g');
    const auto again = atlas.getOrInsert(U'A');
    EXPECT_GT(first.source.w, 0);
    EXPECT_GT(first.source.h, 0);
    EXPECT_GT(first.advanceX, 0);
    EXPECT_EQ(first.source.x, again.source.x);
    EXPECT_EQ(first.source.y, again.source.y);
    EXPECT_EQ(first.bearingY, again.bearingY);
    EXPECT_EQ(first.advanceX, again.advanceX);
    EXPECT_GT(atlas.lineHeight(), 0);
    EXPECT_GT(atlas.ascent(), 0);
}

TEST_CASE("Spaces have an advance without visible bitmap data") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24);
    const auto space = atlas.getOrInsert(U' ');
    EXPECT_GT(space.advanceX, 0);
    EXPECT_EQ(space.source.w, 0);
    EXPECT_EQ(space.source.h, 0);
}

TEST_CASE("Invalid Unicode scalars are rejected") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24);
    EXPECT_THROW(atlas.getOrInsert(char32_t(0x110000)), std::invalid_argument);
    EXPECT_THROW(atlas.getOrInsert(char32_t(0xd800)), std::invalid_argument);
}

TEST_CASE("Missing glyphs use a cached replacement") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24);
    const auto missing = atlas.getOrInsert(char32_t(0x10ffff));
    const auto again = atlas.getOrInsert(char32_t(0x10ffff));
    EXPECT_GT(missing.source.w, 0);
    EXPECT_EQ(missing.source.x, again.source.x);
    EXPECT_EQ(missing.source.y, again.source.y);
}

TEST_CASE("A full atlas fails without invalidating existing glyphs") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24, 64, 64);
    const auto first = atlas.getOrInsert(U'A');
    bool full = false;
    for (char32_t ch = U'B'; ch <= U'~'; ++ch) {
        try {
            atlas.getOrInsert(ch);
        } catch (const std::runtime_error &) {
            full = true;
            break;
        }
    }
    EXPECT(full);
    const auto again = atlas.getOrInsert(U'A');
    EXPECT_EQ(first.source.x, again.source.x);
    EXPECT_EQ(first.source.y, again.source.y);
}

TEST_CASE("Invalid dimensions and missing fonts fail cleanly") {
    Fixture fixture;
    EXPECT_THROW((GlyphAtlas(*fixture.renderer, fixture.font(), 0)), std::invalid_argument);
    EXPECT_THROW((GlyphAtlas(*fixture.renderer, fixture.font(), 24, 0, 64)), std::invalid_argument);
    EXPECT_THROW((GlyphAtlas(*fixture.renderer, "/nonexistent/terminal-font.ttf", 24)), std::runtime_error);
}

TEST_SUIT_END
