#include "mls-unit-test/unittest.h"
#include "sessionrestore.h"

#include <map>

using namespace frontend;

namespace {
struct Pane {
    std::string cwd; // Where this pane's "shell" was told to start.
    std::string title;
    TerminalModel model{20, 4, 10};
    int pid = 0;
};
using Space = Workspace<Pane>;

struct Rig {
    std::string next;           // set by willCreate: the folder for the next pane
    int made = 0;
    std::map<PaneId, SavedPane> info; // what "describe" reports per pane
    Space space{[this]() {
        auto p = std::make_unique<Pane>();
        p->cwd = next;
        p->pid = ++made;
        return p;
    }};
    Rig() { space.setLayoutSpec({{0, 0, 800, 480}, 8, 16, 4, 20, 5}); }
};

SavedPane at(const std::string &cwd, std::vector<std::string> cmd = {}) {
    SavedPane p;
    p.cwd = cwd;
    p.command = std::move(cmd);
    return p;
}
} // namespace

TEST_SUIT_BEGIN(SessionRestore)

TEST_CASE("Capture then restore rebuilds tabs, splits, ratios, titles, focus and folders") {
    Rig a;
    a.space.apply(Command::NewTab);
    a.space.apply(Command::SplitSideBySide);
    const PaneId rightTop = a.space.focusedPaneId();
    a.space.apply(Command::SplitStacked);
    a.space.setParentRatio(rightTop, 0.3);
    a.space.apply(Command::NewTab);
    a.space.setUserTitle(0, "build");
    a.space.apply(Command::SelectTab, 0);
    a.space.apply(Command::FocusLeft);
    int n = 0;
    const auto snap = captureSession(a.space, "work", 1700000000, [&](PaneId, const Pane &) {
        ++n;
        return at("/dir/" + std::to_string(n), n == 2 ? std::vector<std::string>{"vim", "a b"} : std::vector<std::string>{});
    });
    EXPECT(snap.has_value());
    EXPECT_EQ(snap->tabs.size(), std::size_t(2));
    EXPECT_EQ(snap->tabs[0].title, std::string("build"));
    EXPECT_EQ(snap->active, std::size_t(0));
    EXPECT_EQ(snap->tabs[0].focus, std::size_t(0)); // The left pane.
    EXPECT(snap->valid());
    // Through the file format and back.
    const auto parsed = SessionSnapshot::parse(snap->serialize());
    EXPECT(parsed.has_value());

    Rig b;
    const auto report = restoreSession(b.space, *parsed, [&](const SavedPane &p) { b.next = p.cwd; });
    EXPECT_EQ(report.tabs, std::size_t(2));
    EXPECT_EQ(report.refused, std::size_t(0));
    EXPECT_EQ(report.panes.size(), std::size_t(4));
    EXPECT_EQ(b.space.tabCount(), std::size_t(2));
    EXPECT_EQ(b.space.userTitle(0), std::string("build"));
    EXPECT_EQ(b.space.activeIndex(), std::size_t(0));
    // Same shape, same ratios.
    int m = 0;
    const auto again = captureSession(b.space, "work", 1700000000, [&](PaneId, const Pane &p) {
        ++m;
        return at(p.cwd);
    });
    EXPECT(again.has_value());
    EXPECT_EQ(again->tabs[0].focus, std::size_t(0));
    // Every pane was started in the folder saved for it.
    for (const auto &rp : report.panes) {
        const Pane *pane = nullptr;
        b.space.forEachPane([&](PaneId id, const Pane &p, bool) { if (id == rp.id) pane = &p; });
        EXPECT(pane != nullptr);
        EXPECT_EQ(pane->cwd, rp.saved->cwd);
    }
    // The restored shape equals the saved one.
    std::string shapeA, shapeB;
    const auto walk = [](const SavedNode &node, auto &&self, std::string &out) -> void {
        if (node.leaf) { out += "L"; return; }
        out += (node.direction == SplitDirection::SideBySide ? "H" : "V") + std::to_string(node.permille) + "(";
        self(*node.first, self, out);
        out += ",";
        self(*node.second, self, out);
        out += ")";
    };
    for (const auto &t : snap->tabs) walk(*t.root, walk, shapeA);
    for (const auto &t : again->tabs) walk(*t.root, walk, shapeB);
    EXPECT_EQ(shapeA, shapeB);
}

TEST_CASE("Restoring into a workspace that has tabs adds the saved ones after them") {
    Rig a;
    a.space.apply(Command::NewTab);
    a.space.apply(Command::NewTab);
    const auto snap = captureSession(a.space, "s", 1, [](PaneId, const Pane &) { return at("/x"); });
    Rig b;
    b.space.apply(Command::NewTab);
    const auto report = restoreSession(b.space, *snap, [](const SavedPane &) {});
    EXPECT_EQ(b.space.tabCount(), std::size_t(3));
    EXPECT_EQ(report.tabs, std::size_t(2));
    EXPECT_EQ(b.space.activeIndex(), std::size_t(2)); // The saved active tab (the last of two) after the old one.
}

TEST_CASE("Limits are honored: a refused split is counted, the rest comes back") {
    SessionSnapshot s;
    s.name = "tight";
    SavedTab t;
    t.root = SavedNode::split(SplitDirection::SideBySide, 500, SavedNode::leafOf(at("/a")), SavedNode::leafOf(at("/b")));
    s.tabs.push_back(std::move(t));
    Rig r;
    r.space.setMaxPanes(1);
    const auto report = restoreSession(r.space, s, [](const SavedPane &) {});
    EXPECT_EQ(report.tabs, std::size_t(1));
    EXPECT_EQ(report.refused, std::size_t(1));
    EXPECT_EQ(r.space.paneCount(), std::size_t(1));
}

TEST_CASE("The offer to run a command again is text, never an action") {
    const auto p = at("/srv", {"vim", "it's a file"});
    EXPECT_EQ(rerunInput(p), std::string("vim 'it'\\''s a file'"));
    EXPECT(offerText(p).find("was running: vim") == 0);
    EXPECT(offerText(p).find("Enter to run it again") != std::string::npos);
    EXPECT(rerunInput(p).find('\n') == std::string::npos);
    EXPECT(offerText(at("/srv")).empty());
    EXPECT(rerunInput(at("/srv", {"a", "b\nc"})).empty());
}

TEST_CASE("The screen text checkpoint: live rows, trimmed, clean") {
    TerminalModel m(20, 5, 10);
    m.write(U"hello   ");
    m.carriageReturn();
    m.lineFeed();
    m.write(U"wörld");
    EXPECT_EQ(visibleText(m), std::string("hello\nw\xc3\xb6rld"));
    TerminalModel empty(10, 3, 5);
    EXPECT_EQ(visibleText(empty), std::string());
    // Gaps stay, a hostile program cannot smuggle escapes (the model keeps no ESC cells anyway).
    TerminalModel gap(10, 3, 5);
    gap.moveCursor(3, 1);
    gap.write(U"x");
    EXPECT_EQ(visibleText(gap), std::string("\n   x"));
    // It fits the snapshot: a full screen of text is valid.
    TerminalModel big(200, 60, 10);
    for (int i = 0; i < 60; ++i) {
        big.write(std::u32string(199, U'x'));
        big.carriageReturn();
        big.lineFeed();
    }
    SessionSnapshot s;
    s.name = "big";
    SavedTab t;
    SavedPane p;
    p.text = visibleText(big);
    EXPECT(p.text.size() <= SessionSnapshot::kMaxPaneText);
    t.root = SavedNode::leafOf(std::move(p));
    s.tabs.push_back(std::move(t));
    EXPECT(s.valid());
    EXPECT(SessionSnapshot::parse(s.serialize()).has_value());
}

TEST_CASE("Policy: no automatic restore; saves on naming a tab and on shutdown of a saved session only") {
    SessionPolicy p;
    EXPECT(!p.persistent());
    EXPECT(!p.shouldSave(SessionPolicy::Event::Shutdown)); // Never named: nothing is written.
    EXPECT(!p.withScreenText(SessionPolicy::Event::Shutdown));
    EXPECT(p.shouldSave(SessionPolicy::Event::TabNamed));
    EXPECT(p.persistent());
    EXPECT(!p.withScreenText(SessionPolicy::Event::TabNamed)); // The checkpoint is for shutdown only.
    EXPECT(p.shouldSave(SessionPolicy::Event::Shutdown));
    EXPECT(p.withScreenText(SessionPolicy::Event::Shutdown));
    SessionPolicy q("work");
    EXPECT_EQ(q.name(), std::string("work"));
    q.adopt("server");
    EXPECT(q.persistent() && q.name() == "server");
}

TEST_CASE("A user title wins over the program's title in the strip; empty clears it") {
    Rig r;
    r.space.apply(Command::NewTab);
    const auto title = [](const Pane &p) { return p.title.empty() ? std::string("sh") : p.title; };
    EXPECT_EQ(r.space.tabInfos(title)[0].title, std::string("sh"));
    EXPECT(r.space.setUserTitle(0, "mine"));
    EXPECT_EQ(r.space.tabInfos(title)[0].title, std::string("mine"));
    EXPECT(r.space.setUserTitle(0, ""));
    EXPECT_EQ(r.space.tabInfos(title)[0].title, std::string("sh"));
    EXPECT(!r.space.setUserTitle(5, "x"));
}

TEST_SUIT_END
