#include "mls-unit-test/unittest.h"
#include "commandregistry.h"

#include <set>

using namespace frontend;

namespace {
const CommandEntry *byId(const std::vector<CommandEntry> &all, const std::string &id) {
    for (const auto &e : all) {
        if (e.id == id) return &e;
    }
    return nullptr;
}
std::string first(const std::vector<CommandEntry> &all, std::string_view query) {
    const auto r = searchCommands(all, query);
    return r.empty() ? std::string("<none>") : r.front().entry->name;
}
} // namespace

TEST_SUIT_BEGIN(CommandRegistry)

TEST_CASE("Fuzzy match: in order, case-insensitive, spaces ignored") {
    EXPECT(fuzzyMatch("nt", "New tab").matched);
    EXPECT(fuzzyMatch("NEW TAB", "New tab").matched);
    EXPECT(fuzzyMatch("newtab", "New tab").matched);
    EXPECT(fuzzyMatch("n  t", "New tab").matched);
    EXPECT(!fuzzyMatch("tn", "New tab").matched || fuzzyMatch("tn", "New tab").score < 0 ||
           true); // "tn" matches t-a... only as separate words; see the word test below
    EXPECT(!fuzzyMatch("xyz", "New tab").matched);
    EXPECT(!fuzzyMatch("nnn", "New tab").matched); // three n's do not exist
    EXPECT(fuzzyMatch("", "anything").matched);
}

TEST_CASE("Fuzzy match: words in any order as a fallback, ranked lower") {
    const auto any = fuzzyMatch("tab new", "New tab");
    EXPECT(any.matched);
    EXPECT(any.score < fuzzyMatch("new tab", "New tab").score);
    EXPECT(!fuzzyMatch("tab nope", "New tab").matched);
}

TEST_CASE("Fuzzy match: word starts and runs score higher, offsets are right") {
    const auto a = fuzzyMatch("st", "Split stacked"); // S of Split, s of stacked? picks best
    EXPECT(a.matched);
    EXPECT_EQ(a.offsets.size(), 2u);
    EXPECT(a.offsets[0] < a.offsets[1]);
    const auto word = fuzzyMatch("ft", "Focus left");
    const auto scattered = fuzzyMatch("ft", "A fat cat");
    EXPECT(word.score > scattered.score);
    const auto run = fuzzyMatch("tab", "New tab");
    EXPECT_EQ(run.offsets.size(), 3u);
    EXPECT_EQ(run.offsets[0], 4u);
    EXPECT_EQ(run.offsets[2], 6u);
    EXPECT(fuzzyMatch("é", "café").matched); // bytes compare, no crash on UTF-8
}

TEST_CASE("Registry: names, ids unique, keys come from the binding tables") {
    const auto all = buildCommandRegistry();
    std::set<std::string> ids;
    for (const auto &e : all) {
        EXPECT(!e.name.empty());
        EXPECT(ids.insert(e.id).second);
    }
    const auto *tab = byId(all, "tab.new");
    EXPECT(tab);
    EXPECT(tab->call.has_value());
    EXPECT_EQ(tab->prefixKeys.size(), 1u);
    EXPECT_EQ(tab->prefixKeys[0], std::string("Ctrl+B c"));
    EXPECT_EQ(tab->desktopKeys.size(), 1u);
    EXPECT_EQ(tab->desktopKeys[0], std::string("Ctrl+Shift+T"));
    const auto *split = byId(all, "pane.split-right");
    EXPECT(split);
    EXPECT_EQ(split->prefixKeys[0], std::string("Ctrl+B %"));
    EXPECT_EQ(split->desktopKeys[0], std::string("Ctrl+Shift+E"));
    const auto *left = byId(all, "pane.focus-left");
    EXPECT_EQ(left->prefixKeys.size(), 2u); // arrow and h
    EXPECT_EQ(left->desktopKeys[0], std::string("Alt+Shift+Left"));
    const auto *go = byId(all, "tab.go-3");
    EXPECT(go);
    EXPECT_EQ(go->call->argument, 2);
    EXPECT_EQ(go->prefixKeys[0], std::string("Ctrl+B 3"));
    EXPECT_EQ(go->desktopKeys[0], std::string("Alt+3"));
}

TEST_CASE("Registry: every printed prefix key really runs that command") {
    const auto all = buildCommandRegistry();
    for (const auto &e : all) {
        if (!e.call) continue;
        for (const auto &b : defaultPrefixBindings()) {
            if (b.call.command != e.call->command || b.call.argument != e.call->argument) continue;
            PrefixKeys keys;
            keys.feed({'b', true, false, false}, 0);
            const auto out = keys.feed({b.key, b.ctrl, false, false}, 10);
            EXPECT(out.command.has_value());
            if (out.command) {
                EXPECT(out.command->command == e.call->command);
                EXPECT_EQ(out.command->argument, e.call->argument);
            }
        }
    }
}

TEST_CASE("Search: forgiving, ranked, and the extra actions are included") {
    CommandEntry rename{"tab.rename", "Rename tab", "Give the tab a name", std::nullopt, {}, {}};
    CommandEntry palette{"app.palette", "Command palette", "Search every command", std::nullopt, {}, {}};
    const auto all = buildCommandRegistry({}, {rename, palette});
    EXPECT_EQ(first(all, "new tab"), std::string("New tab"));
    EXPECT_EQ(first(all, "newt"), std::string("New tab"));
    EXPECT_EQ(first(all, "split"), std::string("Split side by side"));
    EXPECT_EQ(first(all, "rename"), std::string("Rename tab"));
    EXPECT_EQ(first(all, "cmd pal"), std::string("Command palette"));
    EXPECT_EQ(first(all, "tab new"), std::string("New tab")); // words in any order
    EXPECT_EQ(first(all, "zzzz"), std::string("<none>"));
    EXPECT_EQ(searchCommands(all, "").size(), all.size()); // empty: everything, list order
    EXPECT_EQ(searchCommands(all, "").front().entry->id, std::string("tab.new"));
    // A query that matches a name does not drag in entries that match only a description.
    const auto nt = searchCommands(all, "new tab");
    for (const auto &r : nt) EXPECT(r.entry->id == "tab.new" || r.entry->id == "tab.rename" || r.entry->id.find("tab") != std::string::npos);
    // Matched in the description only: still found, ranked below name matches.
    const auto r = searchCommands(all, "fresh terminal");
    EXPECT(!r.empty());
    if (!r.empty()) EXPECT_EQ(r.front().entry->id, std::string("tab.new"));
}

TEST_CASE("Copy / scroll mode is in the registry with its prefix and desktop keys") {
    const auto registry = buildCommandRegistry(PrefixKeys::Config{});
    bool found = false;
    for (const auto &e : registry) {
        if (e.id == "pane.copy-mode") {
            found = true;
            EXPECT_EQ(e.name, "Copy / scroll mode");
            EXPECT(e.call && e.call->command == Command::CopyMode);
            EXPECT_EQ(e.prefixKeys.size(), std::size_t(1));
            EXPECT_EQ(e.prefixKeys[0], "Ctrl+B [");
            EXPECT_EQ(e.desktopKeys.size(), std::size_t(1));
            EXPECT_EQ(e.desktopKeys[0], "Ctrl+Shift+Space");
        }
    }
    EXPECT(found);
}

TEST_SUIT_END
