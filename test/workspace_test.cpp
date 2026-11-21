#include "mls-unit-test/unittest.h"
#include "workspace.h"

#include <random>
#include <set>
#include <string>
#include <vector>

using namespace frontend;

namespace {

struct Fake {
    static int alive;
    std::string title;
    explicit Fake(std::string t = "") : title(std::move(t)) { ++alive; }
    Fake(const Fake &) = delete;
    Fake &operator=(const Fake &) = delete;
    ~Fake() { --alive; }
};
int Fake::alive = 0;

using Space = Workspace<Fake>;

struct Maker {
    int made = 0;
    bool fail = false;
    Space::Factory factory() {
        return [this]() -> std::unique_ptr<Fake> {
            if (fail) return nullptr;
            return std::make_unique<Fake>("p" + std::to_string(++made));
        };
    }
};

LayoutSpec bigSpec() {
    LayoutSpec s;
    s.area = {0, 0, 3000, 1500};
    s.cellWidth = 8;
    s.cellHeight = 16;
    s.dividerThickness = 6;
    return s;
}

std::vector<std::string> titles(const Space &w) {
    std::vector<std::string> out;
    for (const auto &i : w.tabInfos([](const Fake &f) { return f.title; })) out.push_back(i.title);
    return out;
}

} // namespace

TEST_SUIT_BEGIN(Workspace)

TEST_CASE("The first tab, new tabs after the active one, and selecting") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    EXPECT_EQ(w.tabCount(), std::size_t(0));
    EXPECT(w.apply(Command::NextTab) == Outcome::Ignored);
    EXPECT(w.apply(Command::SplitStacked) == Outcome::Ignored);
    EXPECT(w.apply(Command::ClosePane) == Outcome::Ignored);
    EXPECT(w.apply(Command::EvenOut) == Outcome::Ignored);
    EXPECT(w.apply(Command::NewTab) == Outcome::Done); // p1
    EXPECT(w.apply(Command::NewTab) == Outcome::Done); // p2
    EXPECT(w.apply(Command::SelectTab, 0) == Outcome::Done);
    EXPECT(w.apply(Command::NewTab) == Outcome::Done); // p3 goes after tab 0
    EXPECT_EQ(w.activeIndex(), std::size_t(1));
    EXPECT(titles(w) == (std::vector<std::string>{"p1", "p3", "p2"}));
    EXPECT(w.apply(Command::NextTab) == Outcome::Done);
    EXPECT_EQ(w.activeIndex(), std::size_t(2));
    EXPECT(w.apply(Command::NextTab) == Outcome::Done); // wraps
    EXPECT_EQ(w.activeIndex(), std::size_t(0));
    EXPECT(w.apply(Command::PreviousTab) == Outcome::Done);
    EXPECT_EQ(w.activeIndex(), std::size_t(2));
    EXPECT(w.apply(Command::SelectTab, 2) == Outcome::Ignored); // already there
    EXPECT(w.apply(Command::SelectTab, 3) == Outcome::Ignored);
    EXPECT(w.apply(Command::SelectTab, -1) == Outcome::Ignored);
    const auto info = w.tabInfos([](const Fake &f) { return f.title; });
    EXPECT(info[2].active && !info[0].active && !info[1].active);
    EXPECT(info[0].id != info[1].id && info[1].id != info[2].id && info[0].id != 0);
}

TEST_CASE("A factory that fails changes nothing") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab);
    m.fail = true;
    EXPECT(w.apply(Command::NewTab) == Outcome::Ignored);
    EXPECT(w.apply(Command::SplitSideBySide) == Outcome::Ignored);
    EXPECT_EQ(w.tabCount(), std::size_t(1));
    EXPECT_EQ(w.paneCount(), std::size_t(1));
    Space none(nullptr);
    EXPECT(none.apply(Command::NewTab) == Outcome::Ignored);
}

TEST_CASE("Splitting asks before it makes a pane") {
    Maker m;
    Space w(m.factory());
    LayoutSpec s = bigSpec();
    s.area = {0, 0, 200, 100}; // far too small for two panes
    w.setLayoutSpec(s);
    w.apply(Command::NewTab);
    EXPECT_EQ(m.made, 1);
    EXPECT(w.apply(Command::SplitSideBySide) == Outcome::Ignored);
    EXPECT(w.apply(Command::SplitStacked) == Outcome::Ignored);
    EXPECT_EQ(m.made, 1); // no process was started for the refused splits
    w.setLayoutSpec(bigSpec());
    for (int i = 0; i < 7; ++i) EXPECT(w.apply(i % 2 ? Command::SplitStacked : Command::SplitSideBySide) == Outcome::Done);
    EXPECT_EQ(m.made, 8);
    EXPECT(w.apply(Command::SplitSideBySide) == Outcome::Ignored); // eight is the limit
    EXPECT_EQ(m.made, 8);
    EXPECT_EQ(Fake::alive, 8);
}

TEST_CASE("Closing panes and tabs picks sensible neighbours and reports when empty") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab); // tab A
    w.apply(Command::SplitSideBySide);
    w.apply(Command::NewTab); // tab B
    w.apply(Command::NewTab); // tab C
    EXPECT_EQ(w.activeIndex(), std::size_t(2));
    EXPECT(w.apply(Command::CloseTab) == Outcome::Done); // last: previous becomes active
    EXPECT_EQ(w.activeIndex(), std::size_t(1));
    w.apply(Command::SelectTab, 0);
    // Closing the focused pane of tab A leaves one pane; closing that one closes the tab.
    EXPECT(w.apply(Command::ClosePane) == Outcome::Done);
    EXPECT_EQ(w.tabCount(), std::size_t(2));
    EXPECT_EQ(w.activeTab()->tree.size(), std::size_t(1));
    EXPECT(w.apply(Command::ClosePane) == Outcome::Done);
    EXPECT_EQ(w.tabCount(), std::size_t(1));
    EXPECT_EQ(w.activeIndex(), std::size_t(0));
    EXPECT_EQ(Fake::alive, 1);
    EXPECT(w.apply(Command::ClosePane) == Outcome::Empty);
    EXPECT_EQ(w.tabCount(), std::size_t(0));
    EXPECT_EQ(Fake::alive, 0);
    EXPECT(w.apply(Command::NewTab) == Outcome::Done); // usable again
    EXPECT_EQ(w.activeIndex(), std::size_t(0));
    EXPECT(w.apply(Command::CloseTab) == Outcome::Empty);
    EXPECT_EQ(Fake::alive, 0);
}

TEST_CASE("Closing a tab before the active one keeps the active tab") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    for (int i = 0; i < 4; ++i) w.apply(Command::NewTab); // active is the last one, p4
    EXPECT(w.closeTabAt(0) == Outcome::Done);
    EXPECT_EQ(w.activeIndex(), std::size_t(2));
    EXPECT(titles(w) == (std::vector<std::string>{"p2", "p3", "p4"}));
    EXPECT_EQ(w.focusedPane()->title, std::string("p4"));
    EXPECT(w.closeTabAt(9) == Outcome::Ignored);
}

TEST_CASE("Focus commands move between panes by geometry") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab);
    w.apply(Command::SplitSideBySide);
    w.apply(Command::SplitStacked); // p1 | (p2 over p3), focus on p3
    EXPECT_EQ(w.focusedPane()->title, std::string("p3"));
    EXPECT(w.apply(Command::FocusUp) == Outcome::Done);
    EXPECT_EQ(w.focusedPane()->title, std::string("p2"));
    EXPECT(w.apply(Command::FocusUp) == Outcome::Ignored);
    EXPECT(w.apply(Command::FocusLeft) == Outcome::Done);
    EXPECT_EQ(w.focusedPane()->title, std::string("p1"));
    EXPECT(w.apply(Command::FocusLeft) == Outcome::Ignored);
    EXPECT(w.apply(Command::FocusRight) == Outcome::Done);
    EXPECT(w.apply(Command::FocusDown) == Outcome::Done);
    EXPECT_EQ(w.focusedPane()->title, std::string("p3"));
    // The layout of the active tab matches the tree.
    const Layout l = w.layoutActive();
    EXPECT_EQ(l.panes.size(), std::size_t(3));
    EXPECT_EQ(l.paneAt(1, 1), w.activeTab()->tree.ids()[0]);
    // Clicking a pane focuses it.
    EXPECT(w.focusPane(w.activeTab()->tree.ids()[0]) == Outcome::Done);
    EXPECT_EQ(w.focusedPane()->title, std::string("p1"));
    EXPECT(w.focusPane(w.focusedPaneId()) == Outcome::Ignored);
    EXPECT(w.focusPane(424242) == Outcome::Ignored);
}

TEST_CASE("Activity marks only hidden panes and clears when the tab is shown") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab); // pane 1
    w.apply(Command::SplitSideBySide); // pane 2, same tab
    w.apply(Command::NewTab); // pane 3, active
    const PaneId p1 = w.tab(0)->tree.ids()[0];
    const PaneId p2 = w.tab(0)->tree.ids()[1];
    const PaneId p3 = w.tab(1)->tree.ids()[0];
    EXPECT(!w.noteActivity(p3)); // visible
    EXPECT(!w.paneActivity(p3));
    EXPECT(w.noteActivity(p1)); // hidden
    EXPECT(!w.noteActivity(p1)); // already marked
    EXPECT(w.paneActivity(p1) && !w.paneActivity(p2));
    auto info = w.tabInfos([](const Fake &f) { return f.title; });
    EXPECT(info[0].activity && !info[1].activity);
    EXPECT(!w.noteActivity(12345));
    // Showing the tab clears it.
    w.apply(Command::SelectTab, 0);
    EXPECT(!w.paneActivity(p1));
    info = w.tabInfos([](const Fake &f) { return f.title; });
    EXPECT(!info[0].activity);
    // Ignoring a select of the same tab does not matter; other tab now hidden.
    EXPECT(w.noteActivity(p3));
    w.apply(Command::NextTab);
    EXPECT(!w.paneActivity(p3));
}

TEST_CASE("A parked pane keeps running, belongs to the list, and comes back") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab);
    w.apply(Command::SplitSideBySide);
    const PaneId a = w.tab(0)->tree.ids()[0];
    const PaneId b = w.tab(0)->tree.ids()[1];
    Fake *pa = w.pane(a);
    EXPECT(pa != nullptr);
    EXPECT(w.park(a) == Outcome::Done);
    EXPECT_EQ(Fake::alive, 2); // not destroyed
    EXPECT_EQ(w.parkedCount(), std::size_t(1));
    EXPECT(w.pane(a) == pa); // same object, found in the parked list
    EXPECT_EQ(w.tabOfPane(a), -1);
    EXPECT_EQ(w.tab(0)->tree.size(), std::size_t(1));
    EXPECT(w.park(a) == Outcome::Ignored); // already parked
    // Output of a parked pane is noted.
    EXPECT(w.noteActivity(a));
    EXPECT(w.paneActivity(a));
    // Bring it back next to the focused pane.
    EXPECT(w.unparkSplit(a, SplitDirection::Stacked) == Outcome::Done);
    EXPECT_EQ(w.parkedCount(), std::size_t(0));
    EXPECT_EQ(w.tab(0)->tree.size(), std::size_t(2));
    EXPECT(w.pane(a) == pa);
    // Parking the last pane of a tab closes the tab but keeps the pane.
    w.apply(Command::NewTab);
    const PaneId c = w.focusedPaneId();
    EXPECT(w.park(c) == Outcome::Done);
    EXPECT_EQ(w.tabCount(), std::size_t(1));
    EXPECT_EQ(Fake::alive, 3);
    EXPECT(w.unparkToNewTab(c) == Outcome::Done);
    EXPECT_EQ(w.tabCount(), std::size_t(2));
    EXPECT_EQ(w.focusedPaneId(), c);
    EXPECT(w.unparkToNewTab(c) == Outcome::Ignored);
    // Parking the only pane of the only tab: the workspace is empty, the pane lives.
    EXPECT(w.closeTabAt(1) == Outcome::Done);
    EXPECT(w.park(a) == Outcome::Done);
    EXPECT(w.park(b) == Outcome::Empty);
    EXPECT_EQ(Fake::alive, 2);
    EXPECT_EQ(w.parkedCount(), std::size_t(2));
    EXPECT(w.closeParked(a) == Outcome::Done);
    EXPECT_EQ(Fake::alive, 1);
    EXPECT(w.closeParked(a) == Outcome::Ignored);
}

TEST_CASE("A refused unpark leaves the pane parked") {
    Maker m;
    Space w(m.factory());
    LayoutSpec s = bigSpec();
    w.setLayoutSpec(s);
    w.apply(Command::NewTab);
    w.apply(Command::SplitSideBySide);
    const PaneId a = w.tab(0)->tree.ids()[0];
    w.park(a);
    s.area = {0, 0, 200, 100};
    w.setLayoutSpec(s);
    EXPECT(w.unparkSplit(a, SplitDirection::SideBySide) == Outcome::Ignored);
    EXPECT_EQ(w.parkedCount(), std::size_t(1));
    EXPECT(w.pane(a) != nullptr);
    EXPECT_EQ(Fake::alive, 2);
}

TEST_CASE("Tabs can be moved and the active tab follows") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    for (int i = 0; i < 4; ++i) w.apply(Command::NewTab);
    w.apply(Command::SelectTab, 1); // p2
    EXPECT(w.moveTab(1, 3) == Outcome::Done);
    EXPECT(titles(w) == (std::vector<std::string>{"p1", "p3", "p4", "p2"}));
    EXPECT_EQ(w.activeIndex(), std::size_t(3));
    EXPECT(w.moveTab(0, 2) == Outcome::Done);
    EXPECT(titles(w) == (std::vector<std::string>{"p3", "p4", "p1", "p2"}));
    EXPECT_EQ(w.activeIndex(), std::size_t(3));
    EXPECT(w.moveTab(3, 0) == Outcome::Done);
    EXPECT_EQ(w.activeIndex(), std::size_t(0));
    EXPECT(w.moveTab(1, 1) == Outcome::Ignored);
    EXPECT(w.moveTab(1, 9) == Outcome::Ignored);
    EXPECT_EQ(Fake::alive, 4);
}

TEST_CASE("Divider drag and even out go through the workspace") {
    Maker m;
    Space w(m.factory());
    w.setLayoutSpec(bigSpec());
    w.apply(Command::NewTab);
    w.apply(Command::SplitSideBySide);
    const Layout l = w.layoutActive();
    EXPECT(w.dragDivider(l.dividerSplits[0], 400, false) == Outcome::Done);
    EXPECT(w.layoutActive().panes[0].rect.w != l.panes[0].rect.w);
    EXPECT(w.apply(Command::EvenOut) == Outcome::Done);
    const Layout e = w.layoutActive();
    EXPECT(std::abs(e.panes[0].rect.w - e.panes[1].rect.w) <= 8 + 6);
}

TEST_CASE("Random commands keep every pane owned exactly once") {
    std::mt19937 rng(99);
    for (int round = 0; round < 25; ++round) {
        Maker m;
        const int aliveAtStart = Fake::alive;
        {
            Space w(m.factory());
            LayoutSpec s = bigSpec();
            s.area = {0, 0, 400 + int(rng() % 2600), 150 + int(rng() % 1300)};
            w.setLayoutSpec(s);
            std::set<const Fake *> seen;
            for (int step = 0; step < 150; ++step) {
                switch (rng() % 12) {
                case 0: w.apply(Command::NewTab); break;
                case 1: w.apply(Command::CloseTab); break;
                case 2: w.apply(Command::NextTab); break;
                case 3: w.apply(Command::SelectTab, int(rng() % 5)); break;
                case 4:
                case 5: w.apply(rng() % 2 ? Command::SplitSideBySide : Command::SplitStacked); break;
                case 6: w.apply(Command::ClosePane); break;
                case 7: w.apply(static_cast<Command>(int(Command::FocusLeft) + int(rng() % 4))); break;
                case 8:
                    if (w.focusedPaneId() != 0 && rng() % 2) w.park(w.focusedPaneId());
                    break;
                case 9: {
                    const auto parked = w.parkedIds();
                    if (!parked.empty()) {
                        const PaneId id = parked[rng() % parked.size()];
                        switch (rng() % 3) {
                        case 0: w.unparkToNewTab(id); break;
                        case 1: w.unparkSplit(id, SplitDirection::SideBySide); break;
                        default: w.closeParked(id); break;
                        }
                    }
                    break;
                }
                case 10: {
                    const PaneId id = w.focusedPaneId();
                    if (id != 0) w.noteActivity(id);
                    if (w.tabCount() > 1) w.noteActivity(w.tab((w.activeIndex() + 1) % w.tabCount())->tree.focus());
                    break;
                }
                default: w.apply(Command::EvenOut); break;
                }
                // Invariants: every live pane is reachable exactly once.
                std::set<PaneId> ids;
                std::size_t count = 0;
                w.forEachPane([&](PaneId id, Fake &f, bool) {
                    EXPECT(ids.insert(id).second);
                    EXPECT(&f == w.pane(id));
                    ++count;
                });
                EXPECT_EQ(count, w.paneCount());
                EXPECT_EQ(Fake::alive, aliveAtStart + int(count));
                if (w.tabCount() > 0) {
                    EXPECT(w.activeIndex() < w.tabCount());
                    EXPECT(w.activeTab()->tree.contains(w.focusedPaneId()));
                    for (std::size_t i = 0; i < w.tabCount(); ++i) EXPECT(!w.tab(i)->tree.empty());
                }
            }
        }
        EXPECT_EQ(Fake::alive, aliveAtStart);
    }
}

TEST_SUIT_END
