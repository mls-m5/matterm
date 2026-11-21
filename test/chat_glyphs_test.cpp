// Every glyph the chat view draws beyond ASCII/Latin must exist in the bundled
// terminal font (a missing one shows as a tofu box). Needs CHAT_TEST_FONT (a
// TrueType file); without it the check is skipped.
#include "chatview.h"
#include "mls-unit-test/unittest.h"
#include "tabstrip.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
struct Font {
    std::vector<unsigned char> d;
    std::uint32_t u16(std::size_t o) const {
        return o + 2 <= d.size() ? (d[o] << 8) | d[o + 1] : 0;
    }
    std::uint32_t u32(std::size_t o) const {
        return (u16(o) << 16) | u16(o + 2);
    }
    bool has(char32_t cp) const {
        std::size_t cmap = 0;
        for (std::size_t i = 0, n = u16(4); i < n; ++i)
            if (d.size() >= 12 + 16 * (i + 1) && d[12 + 16 * i] == 'c' &&
                d[13 + 16 * i] == 'm' && d[14 + 16 * i] == 'a' &&
                d[15 + 16 * i] == 'p')
                cmap = u32(12 + 16 * i + 8);
        if (!cmap)
            return false;
        for (std::size_t i = 0, n = u16(cmap + 2); i < n; ++i) {
            const std::size_t t = cmap + u32(cmap + 4 + 8 * i + 4);
            const auto fmt = u16(t);
            if (fmt == 12) {
                for (std::size_t g = 0, ng = u32(t + 12); g < ng; ++g) {
                    const std::size_t e = t + 16 + 12 * g;
                    if (cp >= u32(e) && cp <= u32(e + 4))
                        return true;
                }
            }
            else if (fmt == 4 && cp < 0x10000) {
                const std::size_t segs = u16(t + 6) / 2;
                for (std::size_t s = 0; s < segs; ++s) {
                    const auto end = u16(t + 14 + 2 * s);
                    const auto start = u16(t + 16 + 2 * segs + 2 * s);
                    if (cp >= start && cp <= end) {
                        const auto delta = u16(t + 16 + 4 * segs + 2 * s);
                        const auto range = u16(t + 16 + 6 * segs + 2 * s);
                        if (range == 0)
                            return ((cp + delta) & 0xFFFF) != 0;
                        const auto g = u16(t + 16 + 6 * segs + 2 * s + range +
                                           2 * (cp - start));
                        return g != 0;
                    }
                }
            }
        }
        return false;
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatGlyphs)
TEST_CASE("The font has every glyph the chat view draws") {
    const char *path = std::getenv("CHAT_TEST_FONT");
    if (!path || !*path)
        return;
    std::ifstream in(path, std::ios::binary);
    Font font;
    font.d.assign(std::istreambuf_iterator<char>(in), {});
    EXPECT_GT(font.d.size(), 1000u);
    EXPECT_TRUE(font.has(U'A')); // The parser itself works.
    for (const char32_t cp : chat::kViewGlyphs)
        EXPECT(font.has(cp));
    for (const char32_t cp : frontend::kStripGlyphs)
        EXPECT(font.has(cp));
}
TEST_SUIT_END
