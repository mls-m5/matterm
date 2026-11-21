#include "layoutsnapshot.h"
#include "mls-unit-test/unittest.h"

#include <random>
#include <string>

namespace {
using namespace frontend;

std::string id(int n) {
    std::string s(32, '0');
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        s[std::size_t(31 - i)] = hex[(n >> (4 * i)) & 15];
    }
    return s;
}
std::unique_ptr<SnapshotNode> leaf(int n) {
    return SnapshotNode::pane(id(n));
}
} // namespace

TEST_SUIT_BEGIN(LayoutSnapshot)

TEST_CASE("A snapshot survives serialize and parse, in order") {
    LayoutSnapshot s;
    s.tabs.push_back(leaf(1));
    s.tabs.push_back(SnapshotNode::split(
        SplitDirection::SideBySide,
        300,
        leaf(2),
        SnapshotNode::split(SplitDirection::Stacked, 750, leaf(3), leaf(4))));
    s.active = 1;
    EXPECT(s.valid());
    const auto text = s.serialize();
    const auto back = LayoutSnapshot::parse(text);
    EXPECT(back.has_value());
    EXPECT_EQ(back->serialize(), text);
    EXPECT_EQ(back->active, std::size_t(1));
    EXPECT_EQ(back->tabs.size(), std::size_t(2));
    const auto sessions = back->sessions();
    EXPECT_EQ(sessions.size(), std::size_t(4));
    EXPECT_EQ(sessions[0], id(1));
    EXPECT_EQ(sessions[3], id(4));
    EXPECT(!back->tabs[1]->leaf);
    EXPECT_EQ(back->tabs[1]->permille, 300);
    EXPECT(back->tabs[1]->second->direction == SplitDirection::Stacked);
}

TEST_CASE("Anything else is refused as a whole") {
    LayoutSnapshot s;
    s.tabs.push_back(leaf(1));
    const auto good = s.serialize();
    EXPECT(LayoutSnapshot::parse(good).has_value());
    const std::string bad[] = {
        "",
        "v1;",
        "v2;0;L" + id(1),
        "v1;0",
        "v1;1;L" + id(1),                            // Active tab out of range.
        "v1;0;L" + id(1) + "x",                      // Trailing garbage.
        "v1;0;L" + id(1).substr(1),                  // Short id.
        "v1;0;LA" + id(1).substr(1),                 // Upper case hex.
        "v1;0;H500(L" + id(1) + ")",                 // One child.
        "v1;0;H0(L" + id(1) + ",L" + id(2) + ")",    // Ratio 0.
        "v1;0;H1000(L" + id(1) + ",L" + id(2) + ")", // Ratio 1000.
        "v1;0;X500(L" + id(1) + ",L" + id(2) + ")",
        "v1;0;L" + id(1) + ";L" + id(1), // The same session twice.
        "v1;0;H500(L" + id(1) + ",L" + id(1) + ")",
        "v1;-1;L" + id(1),
        "v1;0;<script>",
        std::string("v1;0;L") + id(1) + std::string(1, '\0'),
    };
    for (const auto &text : bad) {
        EXPECT(!LayoutSnapshot::parse(text).has_value());
    }
}

TEST_CASE("Limits: tabs, panes per tab, panes in all, depth, text size") {
    // Nine panes in one tab.
    auto chain = [&](int first, int count) {
        auto node = leaf(first);
        for (int i = 1; i < count; ++i) {
            node = SnapshotNode::split(SplitDirection::SideBySide,
                                       500,
                                       std::move(node),
                                       leaf(first + i));
        }
        return node;
    };
    LayoutSnapshot s;
    s.tabs.push_back(chain(1, 8));
    EXPECT(s.valid());
    LayoutSnapshot nine;
    nine.tabs.push_back(chain(1, 9));
    EXPECT(!nine.valid());
    EXPECT(!LayoutSnapshot::parse(nine.serialize()).has_value());
    // Seventeen panes over several tabs.
    LayoutSnapshot many;
    for (int i = 0; i < 17; ++i) {
        many.tabs.push_back(leaf(100 + i));
    }
    EXPECT(!many.valid()); // 17 tabs and 17 panes.
    LayoutSnapshot sixteen;
    for (int i = 0; i < 16; ++i) {
        sixteen.tabs.push_back(leaf(100 + i));
    }
    EXPECT(sixteen.valid());
    EXPECT(LayoutSnapshot::parse(sixteen.serialize()).has_value());
    // Too much text, and a tree that is too deep.
    EXPECT(!LayoutSnapshot::parse(std::string(5000, 'x')).has_value());
    std::string deep = "v1;0;";
    for (int i = 0; i < 20; ++i) {
        deep += "H500(";
    }
    EXPECT(!LayoutSnapshot::parse(deep).has_value());
}

TEST_CASE("Fuzz: random and mutated text never crashes and only valid "
          "snapshots come back") {
    LayoutSnapshot s;
    s.tabs.push_back(
        SnapshotNode::split(SplitDirection::Stacked, 400, leaf(1), leaf(2)));
    s.tabs.push_back(leaf(3));
    const std::string good = s.serialize();
    std::mt19937 rng(7);
    const std::string alphabet = "v1;LHV(),0123456789abcdefxX\x01 ";
    for (int i = 0; i < 20000; ++i) {
        std::string text = good;
        const int edits = 1 + int(rng() % 4);
        for (int e = 0; e < edits; ++e) {
            const auto at = rng() % (text.size() + 1);
            switch (rng() % 3) {
            case 0:
                text.insert(at, 1, alphabet[rng() % alphabet.size()]);
                break;
            case 1:
                if (at < text.size()) {
                    text.erase(at, 1);
                }
                break;
            default:
                if (at < text.size()) {
                    text[at] = alphabet[rng() % alphabet.size()];
                }
            }
        }
        const auto parsed = LayoutSnapshot::parse(text);
        if (parsed) {
            EXPECT(parsed->valid());
            EXPECT(LayoutSnapshot::parse(parsed->serialize()).has_value());
        }
    }
}

TEST_SUIT_END
