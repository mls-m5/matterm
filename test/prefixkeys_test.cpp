#include "mls-unit-test/unittest.h"
#include "prefixkeys.h"

#include <set>

using namespace frontend;

namespace {
KeyChord plain(int key) { return {key, false, false, false}; }
KeyChord ctrl(int key) { return {key, true, false, false}; }
const KeyChord kPrefix = ctrl('b');
} // namespace

TEST_SUIT_BEGIN(PrefixKeys)

TEST_CASE("Keys pass through while idle") {
    PrefixKeys k;
    for (int c : {int('a'), int('c'), int('%'), int(keys::Left), int(keys::Enter)}) {
        const KeyOutcome o = k.feed(plain(c), 0);
        EXPECT(o.forwardKey && !o.forwardPrefix && !o.command);
    }
    EXPECT(k.feed(ctrl('c'), 0).forwardKey); // other Ctrl keys are the program's
    EXPECT(k.feed({'b', false, false, false}, 0).forwardKey); // plain b is not the prefix
    EXPECT(k.feed({'b', true, false, true}, 0).forwardKey);   // Ctrl+Alt+B is not either
    EXPECT(!k.waiting());
}

TEST_CASE("The prefix is swallowed and the next key runs the command") {
    PrefixKeys k;
    const KeyOutcome first = k.feed(kPrefix, 100);
    EXPECT(first.swallowed());
    EXPECT(k.waiting());
    const KeyOutcome second = k.feed(plain('c'), 200);
    EXPECT(second.command.has_value());
    EXPECT(second.command->command == Command::NewTab);
    EXPECT(!second.forwardKey && !second.forwardPrefix);
    EXPECT(!k.waiting());
    // And it must be pressed again for the next command.
    EXPECT(k.feed(plain('c'), 300).forwardKey);
}

TEST_CASE("The default bindings") {
    struct Row { KeyChord key; Command command; int arg; };
    const Row rows[] = {
        {plain('c'), Command::NewTab, 0},        {plain('n'), Command::NextTab, 0},
        {plain('p'), Command::PreviousTab, 0},   {plain('1'), Command::SelectTab, 0},
        {plain('7'), Command::SelectTab, 6},     {{'%', false, true, false}, Command::SplitSideBySide, 0},
        {{'"', false, true, false}, Command::SplitStacked, 0}, {plain('x'), Command::ClosePane, 0},
        {plain('&'), Command::CloseTab, 0},      {plain(keys::Left), Command::FocusLeft, 0},
        {plain(keys::Right), Command::FocusRight, 0}, {plain(keys::Up), Command::FocusUp, 0},
        {plain(keys::Down), Command::FocusDown, 0},   {plain('h'), Command::FocusLeft, 0},
        {plain('j'), Command::FocusDown, 0},     {plain('k'), Command::FocusUp, 0},
        {plain('l'), Command::FocusRight, 0},    {plain('='), Command::EvenOut, 0},
        {plain('C'), Command::NewTab, 0}, // letters ignore case
    };
    for (const auto &r : rows) {
        PrefixKeys k;
        k.feed(kPrefix, 0);
        const KeyOutcome o = k.feed(r.key, 1);
        EXPECT(o.command.has_value());
        if (o.command) {
            EXPECT(o.command->command == r.command);
            EXPECT_EQ(o.command->argument, r.arg);
        }
    }
}

TEST_CASE("The prefix twice sends the prefix key itself") {
    PrefixKeys k;
    k.feed(kPrefix, 0);
    const KeyOutcome o = k.feed(kPrefix, 10);
    EXPECT(o.forwardPrefix && !o.forwardKey && !o.command);
    EXPECT(!k.waiting());
    // The third press starts a new wait.
    EXPECT(k.feed(kPrefix, 20).swallowed());
    EXPECT(k.waiting());
}

TEST_CASE("An unknown second key goes to the program together with the prefix") {
    PrefixKeys k;
    k.feed(kPrefix, 0);
    const KeyOutcome o = k.feed(plain('z'), 10);
    EXPECT(o.forwardPrefix && o.forwardKey && !o.command);
    EXPECT(!k.waiting());
    // Ctrl or Alt variants of a bound letter are not the binding.
    k.feed(kPrefix, 20);
    EXPECT(k.feed(ctrl('c'), 21).forwardKey);
    k.feed(kPrefix, 30);
    const KeyOutcome alt = k.feed({'c', false, false, true}, 31);
    EXPECT(alt.forwardKey && alt.forwardPrefix && !alt.command);
}

TEST_CASE("The timeout hands the prefix back to the program") {
    PrefixKeys k;
    k.feed(kPrefix, 1000);
    EXPECT(k.deadline().has_value());
    EXPECT_EQ(*k.deadline(), std::int64_t(3000));
    EXPECT(k.tick(2999).swallowed());
    EXPECT(k.waiting());
    const KeyOutcome o = k.tick(3000);
    EXPECT(o.forwardPrefix && !o.forwardKey && !o.command);
    EXPECT(!k.waiting());
    EXPECT(!k.deadline().has_value());
    EXPECT(k.tick(9999).swallowed()); // nothing more to say
}

TEST_CASE("A key after the timeout is a plain key, preceded by the prefix") {
    PrefixKeys k;
    k.feed(kPrefix, 0);
    const KeyOutcome o = k.feed(plain('c'), 2500); // too late for the command
    EXPECT(o.forwardPrefix && o.forwardKey && !o.command);
    EXPECT(!k.waiting());
    // The prefix after a timeout starts a new wait and still hands back the old prefix.
    k.feed(kPrefix, 0);
    const KeyOutcome again = k.feed(kPrefix, 5000);
    EXPECT(again.forwardPrefix && !again.forwardKey && !again.command);
    EXPECT(k.waiting());
    EXPECT_EQ(*k.deadline(), std::int64_t(7000));
}

TEST_CASE("The prefix key and the timeout are configurable") {
    PrefixKeys::Config c;
    c.prefix = {'a', true, false, false};
    c.timeoutMs = 500;
    c.bindings = {{'q', false, {Command::CloseTab, 0}}};
    PrefixKeys k(c);
    EXPECT(k.feed(kPrefix, 0).forwardKey); // Ctrl+B is just a key now
    EXPECT(k.feed(ctrl('a'), 0).swallowed());
    EXPECT_EQ(*k.deadline(), std::int64_t(500));
    EXPECT(k.feed(plain('c'), 10).forwardKey); // not bound in this table
    k.feed(ctrl('a'), 20);
    const KeyOutcome o = k.feed(plain('q'), 30);
    EXPECT(o.command.has_value() && o.command->command == Command::CloseTab);
    k.feed(ctrl('a'), 40);
    k.reset();
    EXPECT(!k.waiting());
}

TEST_CASE("Random typing: no key other than the prefix is lost or invented") {
    // Every key that is not the prefix reaches the program or runs a command, never both.
    const KeyChord pool[] = {kPrefix, plain('c'), plain('z'), plain('%'), ctrl('c'), plain(keys::Up)};
    for (unsigned seed = 0; seed < 4000; ++seed) {
        unsigned s = seed * 2654435761u + 12345u;
        auto next = [&s] {
            s = s * 1664525u + 1013904223u;
            return s >> 16;
        };
        PrefixKeys k;
        std::int64_t now = 0;
        int others = 0, forwarded = 0, commands = 0;
        for (int i = 0; i < 12; ++i) {
            now += (next() % 4 == 0) ? 3000 : 10;
            const KeyChord key = pool[next() % 6];
            const bool isPrefix = key.ctrl && key.key == 'b';
            if (!isPrefix) ++others;
            const KeyOutcome o = k.feed(key, now);
            if (o.forwardKey) ++forwarded;
            if (o.command) ++commands;
            EXPECT(!(o.forwardKey && o.command));
            if (isPrefix) EXPECT(!o.forwardKey && !o.command);
        }
        EXPECT_EQ(others, forwarded + commands);
        k.tick(now + 100000);
        EXPECT(!k.waiting());
    }
}

TEST_CASE("Desktop shortcuts are GNOME Terminal's, with exact modifiers") {
    auto layout = [](KeyChord k) { return findDesktopShortcut(k); };
    auto t = layout({'t', true, true, false});
    EXPECT(t && t->action == DesktopAction::Layout && t->call.command == Command::NewTab);
    EXPECT(layout({'T', true, true, false}).has_value()); // case folded
    EXPECT(!layout({'t', true, false, false}));            // Ctrl+T is the program's
    EXPECT(!layout({'t', false, true, false}));
    auto w = layout({'w', true, true, false});
    EXPECT(w && w->call.command == Command::CloseTab);
    auto next = layout({keys::PageDown, true, false, false});
    EXPECT(next && next->call.command == Command::NextTab);
    auto prev = layout({keys::PageUp, true, false, false});
    EXPECT(prev && prev->call.command == Command::PreviousTab);
    EXPECT(!layout({keys::PageUp, true, true, false})); // Ctrl+Shift+PgUp is not Ctrl+PgUp
    EXPECT(!layout({keys::PageUp, false, false, false})); // plain PgUp scrolls
    for (int d = 1; d <= 9; ++d) {
        auto s = layout({'0' + d, false, false, true});
        EXPECT(s && s->call.command == Command::SelectTab);
        if (s) EXPECT_EQ(s->call.argument, d - 1);
    }
    EXPECT(!layout({'0', false, false, true}));
    auto copy = layout({'c', true, true, false});
    EXPECT(copy && copy->action == DesktopAction::Copy);
    auto paste = layout({'v', true, true, false});
    EXPECT(paste && paste->action == DesktopAction::Paste);
    EXPECT(!layout({'c', true, false, false})); // Ctrl+C interrupts the program
}

TEST_CASE("Desktop shortcuts never collide with each other or with the prefix") {
    const auto &table = desktopShortcuts();
    std::set<std::tuple<int, bool, bool, bool>> seen;
    for (const auto &s : table) {
        const auto key = std::make_tuple(foldKey(s.chord.key), s.chord.ctrl, s.chord.shift, s.chord.alt);
        EXPECT(seen.insert(key).second);
        EXPECT(s.chord.key != 0);
        // Not a bare Ctrl+key (the program's) and not the prefix.
        EXPECT(s.chord.shift || s.chord.alt || s.chord.key >= keys::Up);
        EXPECT(!(foldKey(s.chord.key) == 'b' && s.chord.ctrl && !s.chord.shift && !s.chord.alt));
    }
    // Every layout command is reachable from the desktop table except SelectTab 10+.
    std::set<int> commands;
    for (const auto &s : table) {
        if (s.action == DesktopAction::Layout) commands.insert(int(s.call.command));
    }
    for (int c = int(Command::NewTab); c <= int(Command::EvenOut); ++c) EXPECT(commands.count(c) == 1);
}

TEST_CASE("Every layout command is reachable from the prefix table") {
    std::set<int> commands;
    for (const auto &b : defaultPrefixBindings()) commands.insert(int(b.call.command));
    for (int c = int(Command::NewTab); c <= int(Command::EvenOut); ++c) EXPECT(commands.count(c) == 1);
    std::set<std::pair<int, bool>> keys;
    for (const auto &b : defaultPrefixBindings()) EXPECT(keys.insert({foldKey(b.key), b.ctrl}).second);
}

TEST_SUIT_END
