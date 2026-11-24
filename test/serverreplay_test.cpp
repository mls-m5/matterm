#include "mls-unit-test/unittest.h"
#include "replayring.h"
#include <limits>

TEST_SUIT_BEGIN(ServerReplay)
TEST_CASE(
    "Replay is bounded including chunk overhead and retains absolute offsets") {
    server::ReplayRing ring(1024);
    for (unsigned i = 0; i < 80; ++i) {
        ring.append(
            protocol::Time(i), std::string(40, char('A' + i % 26)) + "\n", i);
        EXPECT(ring.cost() <= 1024);
        EXPECT_EQ(ring.end(), std::uint64_t((i + 1) * 41));
    }
    EXPECT(ring.oldest() > 0);
    auto tail = ring.read(ring.oldest(), 1024);
    EXPECT(tail.has_value());
    EXPECT_EQ(tail->size(), std::size_t(ring.end() - ring.oldest()));
    EXPECT_FALSE(ring.read(ring.oldest() - 1, 10).has_value());
    EXPECT(ring.safeStart() >= ring.oldest());
    EXPECT(ring.safeStart() <= ring.end());
    EXPECT(ring.oldestTime() <= protocol::Time(79));
    ring.clear();
    EXPECT_EQ(ring.cost(), std::size_t(0));
    EXPECT_EQ(ring.oldest(), ring.end());
    EXPECT_FALSE(ring.read(0, 1024).has_value());
}
TEST_CASE("Zero buffer stores nothing, huge chunks fit, and no newline means "
          "drop replay") {
    server::ReplayRing disabled(0);
    disabled.append(protocol::Time(0), "secret", 0);
    EXPECT_EQ(disabled.cost(), std::size_t(0));
    EXPECT_EQ(disabled.end(), std::uint64_t(6));
    server::ReplayRing ring(512);
    ring.append(protocol::Time(0), std::string(4096, 'x'), 0);
    EXPECT(ring.cost() <= 512);
    EXPECT_EQ(ring.end(), std::uint64_t(4096));
    EXPECT_EQ(ring.safeStart(), ring.end());
    ring.append(protocol::Time(1), "\nnext", 1);
    EXPECT(ring.safeStart() < ring.end());
    EXPECT_EQ(ring.read(ring.safeStart(), 100).value(), std::string("next"));
}
TEST_CASE(
    "Oldest eviction is chunked and sequence order breaks timestamp ties") {
    server::ReplayRing a(4096), b(4096);
    a.append(protocol::Time(1), "a\n", 4);
    b.append(protocol::Time(1), "b\n", 5);
    EXPECT(a.oldestSerial() < b.oldestSerial());
    a.dropOldest();
    EXPECT_EQ(a.cost(), std::size_t(0));
    EXPECT_EQ(a.oldest(), a.end());
}
TEST_CASE("Newest retained output time is O1 metadata, unknown without "
          "retained chunks") {
    server::ReplayRing ring(4096), disabled(0);
    EXPECT(!ring.newestTime());
    ring.append(protocol::Time(12), "abc", 1);
    EXPECT(ring.newestTime() == protocol::Time(12));
    ring.append(protocol::Time(44), "def", 2);
    EXPECT(ring.newestTime() == protocol::Time(44));
    EXPECT(ring.read(3, 3) == std::string("def"));
    EXPECT(ring.newestTime() == protocol::Time(44));
    disabled.append(protocol::Time(44), "not retained", 1);
    EXPECT(!disabled.newestTime());
    ring.clear();
    EXPECT(!ring.newestTime());
}
TEST_CASE("Replay offset lookup preserves all boundaries while locating late "
          "chunks efficiently") {
    server::ReplayRing ring(2u << 20);
    std::string all;
    for (unsigned i = 0; i < 4000; ++i) {
        const std::string chunk(1 + i % 4, char('a' + i % 26));
        all += chunk;
        ring.append(protocol::Time(i), chunk, i);
    }
    for (std::size_t i = 0; i <= all.size(); ++i)
        EXPECT(ring.read(i, 19) == all.substr(i, 19));
    EXPECT(ring.read(ring.end(), 0) == std::string());
}
TEST_SUIT_END
