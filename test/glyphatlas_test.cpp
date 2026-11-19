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
        SDL_CreateRGBSurfaceWithFormat(0, 128, 128, 32, SDL_PIXELFORMAT_RGBA32),
        SDL_FreeSurface};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
        surface ? SDL_CreateSoftwareRenderer(surface.get()) : nullptr,
        SDL_DestroyRenderer};

    Fixture() {
        if (!renderer) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    const char *font() const {
        const char *path = std::getenv("GLYPH_TEST_FONT");
        if (!path || !*path) {
            throw std::runtime_error(
                "Set GLYPH_TEST_FONT to a TrueType font file");
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

TEST_CASE(
    "Zero-width rendered text is cached without allocating an atlas slot") {
    Fixture fixture;
    std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font{
        TTF_OpenFont(fixture.font(), 24), TTF_CloseFont};
    EXPECT(bool(font));
    EXPECT_GT(TTF_GlyphIsProvided32(font.get(), U'\u00ad'), 0);
    for (const auto style :
         {CellAttribute::None, CellAttribute::Bold, CellAttribute::Italic}) {
        TTF_SetFontStyle(
            font.get(),
            (hasAttribute(style, CellAttribute::Bold) ? TTF_STYLE_BOLD : 0) |
                (hasAttribute(style, CellAttribute::Italic) ? TTF_STYLE_ITALIC
                                                            : 0));
        int width = -1, advance = -1;
        EXPECT_EQ(TTF_SizeUTF8(font.get(), "\xc2\xad", &width, nullptr), 0);
        EXPECT_EQ(TTF_GlyphMetrics32(font.get(),
                                     U'\u00ad',
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     &advance),
                  0);
        GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24, 64, 64);
        // SDL_ttf 2.20.1 reports nonempty glyph metrics for soft hyphen,
        // but regular RenderGlyph32_Blended fails with "Text has zero width".
        // Synthetic styles can add padding: measure each style separately.
        const auto glyph = atlas.getOrInsert(U'\u00ad', style);
        const auto again = atlas.getOrInsert(U'\u00ad', style);
        EXPECT_EQ(glyph.advanceX, advance);
        EXPECT_EQ(glyph.advanceX, again.advanceX);
        EXPECT_EQ(glyph.source.x, again.source.x);
        EXPECT_EQ(glyph.source.y, again.source.y);
        if (width == 0) {
            EXPECT_EQ(glyph.source.w, 0);
            EXPECT_EQ(glyph.source.h, 0);
        }
        // Invisible text must not consume shelf space or break subsequent
        // glyphs.
        const auto visible = atlas.getOrInsert(U'-');
        EXPECT_GT(visible.source.w, 0);
        if (width == 0) {
            EXPECT_EQ(visible.source.x, 1);
            EXPECT_EQ(visible.source.y, 1);
        }
    }
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

TEST_CASE(
    "System font fallback supplies distinct Braille glyphs within a cell") {
    // Opt in with a primary font without Braille and an installed font that
    // covers it; minimal/headless builds need not have any system fonts.
    if (!std::getenv("GLYPH_TEST_EXPECT_BRAILLE_FALLBACK")) {
        return;
    }
    Fixture fixture;
    std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> primary{
        TTF_OpenFont(fixture.font(), 18), TTF_CloseFont};
    if (!primary) {
        throw std::runtime_error(TTF_GetError());
    }
    EXPECT_EQ(TTF_GlyphIsProvided32(primary.get(), U'\u2801'), 0);
    EXPECT_EQ(TTF_GlyphIsProvided32(primary.get(), U'\u2880'), 0);
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 18);
    const int cellWidth = atlas.getOrInsert(U'M').advanceX;
    const auto first = atlas.getOrInsert(U'\u2801');
    const auto last = atlas.getOrInsert(U'\u2880');
    const auto replacement = atlas.getOrInsert(char32_t(0x10ffff));
    EXPECT_GT(first.source.w, 0);
    EXPECT_GT(last.source.w, 0);
    EXPECT(first.source.w <= cellWidth);
    EXPECT(first.source.h <= atlas.lineHeight());
    EXPECT_EQ(first.advanceX, cellWidth);
    EXPECT(first.source.x != last.source.x || first.source.y != last.source.y);
    EXPECT(first.source.x != replacement.source.x ||
           first.source.y != replacement.source.y);
    const auto again = atlas.getOrInsert(U'\u2801');
    EXPECT_EQ(first.source.x, again.source.x);
    EXPECT_EQ(first.source.y, again.source.y);
}

TEST_CASE("A full atlas fails without invalidating existing glyphs") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24, 64, 64);
    const auto first = atlas.getOrInsert(U'A');
    bool full = false;
    for (char32_t ch = U'B'; ch <= U'~'; ++ch) {
        try {
            atlas.getOrInsert(ch);
        }
        catch (const std::runtime_error &error) {
            EXPECT_EQ(std::string(error.what()), "Glyph atlas is full");
            full = true;
            break;
        }
    }
    EXPECT(full);
    const auto again = atlas.getOrInsert(U'A');
    EXPECT_EQ(first.source.x, again.source.x);
    EXPECT_EQ(first.source.y, again.source.y);
}

TEST_CASE("A glyph larger than its atlas still reports an error") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24, 2, 2);
    try {
        atlas.getOrInsert(U'A');
        EXPECT(false);
    }
    catch (const std::runtime_error &error) {
        EXPECT_EQ(std::string(error.what()), "Glyph is larger than the atlas");
    }
}

TEST_CASE("Invalid dimensions and missing fonts fail cleanly") {
    Fixture fixture;
    EXPECT_THROW((GlyphAtlas(*fixture.renderer, fixture.font(), 0)),
                 std::invalid_argument);
    EXPECT_THROW((GlyphAtlas(*fixture.renderer, fixture.font(), 24, 0, 64)),
                 std::invalid_argument);
    EXPECT_THROW(
        (GlyphAtlas(*fixture.renderer, "/nonexistent/terminal-font.ttf", 24)),
        std::runtime_error);
}

TEST_CASE("Font style variants have distinct stable atlas slots but "
          "decorations do not") {
    Fixture fixture;
    GlyphAtlas atlas(*fixture.renderer, fixture.font(), 24);
    const auto regular = atlas.getOrInsert(U'A');
    const auto bold = atlas.getOrInsert(U'A', CellAttribute::Bold);
    const auto italic = atlas.getOrInsert(U'A', CellAttribute::Italic);
    EXPECT(regular.source.x != bold.source.x ||
           regular.source.y != bold.source.y);
    EXPECT(italic.source.x != bold.source.x ||
           italic.source.y != bold.source.y);
    const auto decorated = atlas.getOrInsert(
        U'A', CellAttribute::Underline | CellAttribute::Faint);
    EXPECT_EQ(decorated.source.x, regular.source.x);
    EXPECT_EQ(decorated.source.y, regular.source.y);
    const auto again =
        atlas.getOrInsert(U'A', CellAttribute::Bold | CellAttribute::Underline);
    EXPECT_EQ(again.source.x, bold.source.x);
    EXPECT_EQ(again.source.y, bold.source.y);
}

TEST_SUIT_END
