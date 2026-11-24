#include "control.h"
#include "mls-unit-test/unittest.h"
#include "previewcache.h"
#include "replayring.h"

namespace {
using protocol::Time;
struct Source {
    server::ReplayRing ring;
    unsigned reads = 0;
    explicit Source(std::size_t budget = 4u << 20)
        : ring(budget) {}
    void append(std::string_view text, int ms = 0) {
        ring.append(Time(ms), text, 1);
    }
    std::optional<std::string> get(mtd::PreviewCache &cache,
                                   const std::string &id,
                                   int now,
                                   bool on = true) {
        return cache.get(
            id,
            ring.oldest(),
            ring.end(),
            [&](std::uint64_t offset, std::size_t limit) {
                ++reads;
                return ring.read(offset, limit);
            },
            Time(now),
            on);
    }
};
} // namespace
TEST_SUIT_BEGIN(PreviewCache)
TEST_CASE(
    "Preview off never reads or decodes and clears any existing text cache") {
    mtd::PreviewCache cache;
    Source source;
    source.append("private text");
    EXPECT(!source.get(cache, "a", 0, false));
    EXPECT(source.reads == 0 && cache.size() == 0 && cache.decodedBytes() == 0);
    EXPECT(source.get(cache, "a", 0) == "private text");
    const auto decoded = cache.decodedBytes();
    const auto reads = source.reads;
    EXPECT(!source.get(cache, "a", 1, false));
    EXPECT(source.reads == reads && cache.size() == 0 &&
           cache.decodedBytes() == decoded);
    Source empty;
    const auto knownEmpty = empty.get(cache, "empty", 2);
    EXPECT(knownEmpty && knownEmpty->empty());
}
TEST_CASE("Bounded fragments decode existing VT, keep Unicode, and never "
          "expose strings or bidi") {
    mtd::PreviewCache cache;
    Source source;
    source.append("first\r\n\x1b[32m服务器 å\x1b[0m");
    EXPECT(source.get(cache, "a", 0) == "服务器 å");
    source.append("\r\n\x1b]52;c;secret\ninside\a\x1bPsecret\x1b\\safe");
    EXPECT(source.get(cache, "a", 1) == "safe");
    source.append("\r\nunsafe\xe2\x80\xae"
                  "text");
    EXPECT(!source.get(cache, "a", 2));
    source.append("\r\nrecovered\twords");
    EXPECT(source.get(cache, "a", 3) == "recovered words");
    source.append("\r\n" + std::string(500, 'x'));
    const auto bounded = source.get(cache, "a", 4);
    EXPECT(bounded && bounded->size() == 256 &&
           *bounded == std::string(256, 'x'));
    std::string emoji;
    for (int i = 0; i < 300; ++i)
        emoji += "🔥";
    source.append("\r\n" + emoji);
    const auto wide = source.get(cache, "a", 5);
    EXPECT(wide && wide->size() == 1024 && mtd::validPreviewText(*wide));
}
TEST_CASE("Cold evicted tails never trust LF BEL or an old fragment as fresh") {
    mtd::PreviewCache cache;
    Source source(4096);
    source.append("\x1b]52;c;" + std::string(7000, 's') + "\nhidden\aSECRET");
    EXPECT(source.ring.oldest() > 0);
    EXPECT(!source.get(cache, "a", 0));
    source.append("\x1b\\safe");
    EXPECT(source.get(cache, "a", 1) == "safe");
    source.append(std::string(10000, 'n'));
    EXPECT(!source.get(
        cache, "a", 2)); // Continuation was evicted; safe is not fresh.
    source.append("\x1b\\again");
    EXPECT(source.get(cache, "a", 3) == "again");
}
TEST_CASE("Unprocessed tail and split UTF8 are unavailable until caught up") {
    mtd::PreviewCache cache;
    Source source;
    source.append("old\n");
    EXPECT(source.get(cache, "a", 0) == "old");
    source.append(std::string(16383, 'x') + "å");
    EXPECT(!source.get(cache, "a", 1));
    const auto text = source.get(cache, "a", 1000);
    EXPECT(text && text->size() == 257 &&
           text->substr(text->size() - 2) == "å");
    EXPECT(mtd::validPreviewText(*text));
}
TEST_CASE("Decode budget is global, fixed memory is LRU bounded, and TTL "
          "really expires") {
    mtd::PreviewCache cache;
    Source source;
    source.append(std::string(32768, 'x'));
    for (int i = 0; i < 4; ++i)
        EXPECT(!source.get(cache, std::to_string(i), 0));
    EXPECT(cache.decodedBytes() == 65536);
    const auto reads = source.reads;
    EXPECT(!source.get(cache, "5", 0));
    EXPECT(source.reads == reads);
    EXPECT(!source.get(cache, "5", 1, false));
    EXPECT(!source.get(cache, "5", 1));
    EXPECT(source.reads == reads); // off cannot refill global budget
    EXPECT(source.get(cache, "0", 1000).has_value());
    EXPECT(cache.decodedBytes() == 81920);
    cache.expire(Time(6000));
    EXPECT(cache.size() == 0 && !cache.nextExpiry());
    Source small;
    small.append("ready");
    for (int i = 0; i < 16; ++i)
        EXPECT(small.get(cache, std::to_string(i), 7000 + i) == "ready");
    EXPECT(cache.size() == 16);
    EXPECT(small.get(cache, "0", 7020) == "ready");
    EXPECT(small.get(cache, "16", 7021) == "ready");
    EXPECT(cache.size() == 16);
    const auto before = cache.decodedBytes();
    EXPECT(small.get(cache, "1", 7022) == "ready");
    EXPECT(cache.decodedBytes() == before + 5);
    cache.expire(Time(12022));
    EXPECT(cache.size() == 0 && !cache.nextExpiry());
}
TEST_CASE(
    "LRU uses request order even when every access has the same millisecond") {
    mtd::PreviewCache cache;
    Source source;
    source.append("ready");
    for (int i = 0; i < 16; ++i)
        EXPECT(source.get(cache, (i < 10 ? "0" : "") + std::to_string(i), 0) ==
               "ready");
    EXPECT(source.get(cache, "00", 0) == "ready");
    EXPECT(source.get(cache, "16", 0) == "ready");
    const auto before = cache.decodedBytes();
    EXPECT(source.get(cache, "00", 0) == "ready");
    EXPECT(cache.decodedBytes() ==
           before); // Most recent was NOT evicted by key order.
    EXPECT(source.get(cache, "01", 0) == "ready");
    EXPECT(cache.decodedBytes() == before + 5);
}
TEST_CASE(
    "Large retained backlogs restart at a bounded unsynchronized recent tail") {
    mtd::PreviewCache cache;
    Source source;
    source.append("old\r\n");
    EXPECT(source.get(cache, "a", 0) == "old");
    source.append(std::string(100000, 'x') + "\x1b\\fresh");
    EXPECT(source.ring.oldest() == 0);
    EXPECT(source.get(cache, "a", 1) == "fresh");
    source.append("\x1b]52;" + std::string(100000, 's') + "\nHIDDEN\aSECRET");
    EXPECT(
        !source.get(cache, "a", 2)); // No LF/BEL false-sync, no old fresh text.
    source.append("\x1b\\latest");
    EXPECT(source.get(cache, "a", 3) == "latest");
    source.append("\x1bP" + std::string(100000, 'd') + "\nHIDDEN\aSECRET");
    EXPECT(!source.get(cache, "a", 4));
    source.append("\x1b\\dcslatest");
    EXPECT(source.get(cache, "a", 5) == "dcslatest");
    EXPECT(cache.decodedBytes() <= 5 + 3 * mtd::PreviewCache::kReadBytes + 19);
}
TEST_SUIT_END
