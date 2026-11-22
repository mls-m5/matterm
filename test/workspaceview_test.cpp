#include "mls-unit-test/unittest.h"
#include "ui/commandpalette.h"
#include "workspaceview.h"

#include <set>
#include <string>
#include <vector>

using namespace frontend;

namespace {

class PushLink final : public TerminalLink {
public:
    void setEvents(Events e) override { events_ = std::move(e); }
    bool sendInput(std::string_view b) override {
        sent.append(b);
        return true;
    }
    void resize(int c, int r, int, int) override { resizes.push_back({c, r}); }
    void close() override {}
    void push(const std::string &text) {
        if (events_.output) events_.output(text);
    }
    std::string sent;
    std::vector<std::pair<int, int>> resizes;

private:
    Events events_;
};

struct Pane {
    PushLink link; // before the view: the view borrows it
    TerminalView v;
    explicit Pane(std::string title = "") {
        v.attach(link);
        if (!title.empty()) link.push("\x1b]0;" + title + "\x07");
    }
    TerminalView &view() { return v; }
    const TerminalView &view() const { return v; }
};

using Space = Workspace<Pane>;
using View = WorkspaceView<Pane>;

struct Rig {
    int made = 0;
    Space space{[this]() { return std::make_unique<Pane>("t" + std::to_string(++made)); }};
    View view{space};
    Rig(int w = 1200, int h = 800) {
        view.setWindow({w, h, 8, 16});
        view.key({'t', true, true, false}, 0); // first tab
        view.sync(0);
    }
    MouseInput at(int x, int y, MouseInput::Type t = MouseInput::Type::Down,
                  MouseInput::Button b = MouseInput::Button::Left, bool shift = false) {
        MouseInput m;
        m.x = x;
        m.y = y;
        m.type = t;
        m.button = b;
        m.shift = shift;
        return m;
    }
    void split() { view.key({'e', true, true, false}, 0); }
};

const KeyChord kPrefix{'b', true, false, false};

} // namespace

TEST_SUIT_BEGIN(WorkspaceView)

TEST_CASE("One pane fills the window below the strip, keeping the scrollbar strip") {
    Rig r(1200, 800);
    const FramePlan p = r.view.plan();
    EXPECT_EQ(p.stripRect.h, 16);
    EXPECT_EQ(p.stripRect.w, 1200);
    EXPECT_EQ(p.strip.cells.size(), std::size_t(150));
    EXPECT_EQ(p.panes.size(), std::size_t(1));
    const PanePlan &a = p.panes[0];
    EXPECT(a.rect == (PixelRect{0, 16, 1200, 784}));
    EXPECT_EQ(a.columns, (1200 - 12) / 8);
    EXPECT_EQ(a.rows, 784 / 16);
    EXPECT_EQ(a.scrollbar.w, 0); // edge pane: strip inside
    EXPECT(a.focused);
    // sync() gave the program that size.
    EXPECT_EQ(r.space.focusedPane()->view().model().columns(), std::size_t(a.columns));
    EXPECT_EQ(r.space.focusedPane()->view().model().rows(), std::size_t(a.rows));
    EXPECT(!r.view.sync(1)); // nothing more to do
    EXPECT(!r.space.focusedPane()->link.resizes.empty());
}

TEST_CASE("The strip can hide with one tab") {
    Space space([] { return std::make_unique<Pane>(); });
    ViewConfig c;
    c.showStripWithOneTab = false;
    View v(space, c);
    v.setWindow({800, 480, 8, 16});
    v.key({'t', true, true, false}, 0);
    EXPECT_EQ(v.plan().stripRect.h, 0);
    EXPECT_EQ(v.plan().panes[0].rect.y, 0);
    v.key({'t', true, true, false}, 0);
    EXPECT_EQ(v.plan().stripRect.h, 16);
    EXPECT_EQ(v.plan().panes[0].rect.y, 16);
}

TEST_CASE("A split gives the left pane a scrollbar in the gutter and keeps ratios on resize") {
    Rig r;
    r.split();
    r.view.sync(0);
    FramePlan p = r.view.plan();
    EXPECT_EQ(p.panes.size(), std::size_t(2));
    EXPECT_EQ(p.dividers.size(), std::size_t(1));
    const PanePlan &left = p.panes[0];
    const PanePlan &right = p.panes[1];
    EXPECT_EQ(left.scrollbar.x, p.dividers[0].rect.x);
    EXPECT_EQ(left.scrollbar.w, p.dividers[0].rect.w / 2);
    EXPECT_EQ(left.scrollbar.y, left.rect.y);
    EXPECT_EQ(left.scrollbar.h, left.rect.h);
    EXPECT_EQ(right.scrollbar.w, 0);
    EXPECT_EQ(left.columns, left.rect.w / 8); // no strip taken from it
    EXPECT_EQ(right.columns, (right.rect.w - 12) / 8);
    EXPECT(right.focused); // the new pane has the focus
    // The program sizes follow.
    EXPECT_EQ(r.space.pane(left.id)->view().model().columns(), std::size_t(left.columns));
    EXPECT_EQ(r.space.pane(right.id)->view().model().columns(), std::size_t(right.columns));
    // Make the window much wider: still about half and half.
    r.view.setWindow({2400, 900, 8, 16});
    r.view.sync(10);
    p = r.view.plan();
    EXPECT(std::abs(p.panes[0].rect.w - p.panes[1].rect.w) <= 2);
    EXPECT_EQ(p.panes[0].rect.w + p.dividers[0].rect.w + p.panes[1].rect.w, 2400);
    EXPECT_EQ(r.space.pane(p.panes[1].id)->view().model().columns(), std::size_t(p.panes[1].columns));
    // Shrink to half again: ratio still held.
    r.view.setWindow({1200, 900, 8, 16});
    p = r.view.plan();
    EXPECT(std::abs(p.panes[0].rect.w - p.panes[1].rect.w) <= 2);
}

TEST_CASE("Keys: desktop shortcuts, prefix keys and pass-through") {
    Rig r;
    // Desktop: Ctrl+Shift+E splits.
    KeyResult k = r.view.key({'e', true, true, false}, 0);
    EXPECT(k.changed && !k.forwardKey);
    EXPECT_EQ(k.commands.size(), std::size_t(1));
    EXPECT(k.commands[0].command == Command::SplitSideBySide);
    EXPECT_EQ(r.space.activeTab()->tree.size(), std::size_t(2));
    // Plain keys pass through.
    k = r.view.key({'a', false, false, false}, 5);
    EXPECT(k.forwardKey && !k.changed && k.commands.empty());
    // Prefix + c opens a tab.
    k = r.view.key(kPrefix, 10);
    EXPECT(!k.forwardKey && !k.forwardPrefix && !k.changed);
    EXPECT(r.view.nextWakeMs().has_value());
    k = r.view.key({'c', false, false, false}, 20);
    EXPECT(k.changed);
    EXPECT_EQ(r.space.tabCount(), std::size_t(2));
    EXPECT(!r.view.nextWakeMs().has_value());
    // Prefix, unknown key: the program gets both.
    r.view.key(kPrefix, 30);
    k = r.view.key({'z', false, false, false}, 31);
    EXPECT(k.forwardPrefix && k.forwardKey);
    // Prefix timeout.
    r.view.key(kPrefix, 40);
    EXPECT(!r.view.tick(100).forwardPrefix);
    EXPECT(r.view.tick(2040).forwardPrefix);
    // Copy and paste are reported.
    EXPECT(r.view.key({'c', true, true, false}, 3000).copy);
    EXPECT(r.view.key({'v', true, true, false}, 3000).paste);
    // Desktop shortcuts can be switched off.
    Space space([] { return std::make_unique<Pane>(); });
    ViewConfig c;
    c.desktopShortcuts = false;
    View v(space, c);
    v.setWindow({800, 480, 8, 16});
    EXPECT(v.key({'t', true, true, false}, 0).forwardKey);
    EXPECT_EQ(space.tabCount(), std::size_t(0));
}

TEST_CASE("A held shortcut acts once and held plain keys never start the prefix") {
        Rig r;
        KeyResult k = r.view.key({'e', true, true, false}, 0, false);
        EXPECT(k.changed);
        k = r.view.key({'e', true, true, false}, 10, true);
        EXPECT(!k.changed && !k.forwardKey && !k.forwardPrefix && k.commands.empty());
        EXPECT_EQ(r.space.activeTab()->tree.size(), std::size_t(2));
        k = r.view.key(kPrefix, 20, true); // held Ctrl+B is forwarded, not the prefix
        EXPECT(k.forwardKey);
        EXPECT(!r.view.prefixWaiting());
    }

    TEST_CASE("setMaxPanes stops new tabs and splits at the limit") {
        Rig r;
        r.space.setMaxPanes(3);
        r.view.key({'e', true, true, false}, 0); // 2 panes
        r.view.key({'t', true, true, false}, 1); // new tab: 3 panes
        EXPECT_EQ(r.space.paneCount(), std::size_t(3));
        const KeyResult newTab = r.view.key({'t', true, true, false}, 2);
        EXPECT(!newTab.changed);
        EXPECT_EQ(newTab.refused.size(), std::size_t(1)); // The owner can say why.
        const KeyResult split = r.view.key({'e', true, true, false}, 3);
        EXPECT(!split.changed);
        EXPECT_EQ(split.refused.size(), std::size_t(1));
        EXPECT(r.view.key({'e', true, true, false}, 4).commands.empty());
        EXPECT_EQ(r.space.paneCount(), std::size_t(3));
    }

    TEST_CASE("Closing a pane or tab that runs a program asks first; Cancel changes nothing") {
        Rig r;
        std::set<PaneId> busy;
        r.view.setBusyProbe([&busy](PaneId id) -> std::optional<std::string> {
            if (!busy.count(id)) return std::nullopt;
            return "vim notes.txt";
        });
        r.split();
        r.view.key({'t', true, true, false}, 0); // second tab
        const PaneId focused = r.space.focusedPaneId();
        // Nothing runs: closes at once, as before.
        KeyResult k = r.view.key({'w', false, true, true}, 1); // ClosePane (not busy) -> closes tab 2
        EXPECT(k.changed && !k.confirm);
        EXPECT_EQ(r.space.tabCount(), std::size_t(1));
        // A busy focused pane in a split: the key asks.
        const PaneId pane = r.space.focusedPaneId();
        busy.insert(pane);
        k = r.view.key({'w', false, true, true}, 2);
        EXPECT(!k.changed && k.confirm.has_value());
        EXPECT(k.confirm->scope == CloseScope::Pane && k.confirm->pane == pane);
        EXPECT_EQ(k.confirm->running.size(), std::size_t(1));
        EXPECT(k.confirm->question().find("vim notes.txt") != std::string::npos);
        EXPECT_EQ(r.space.paneCount(), std::size_t(2));
        EXPECT(r.view.pendingClose().has_value());
        // Cancel: still there, nothing pending.
        KeyResult a = r.view.answerClose(CloseAnswer::Cancel);
        EXPECT(!a.changed && !r.view.pendingClose().has_value());
        EXPECT_EQ(r.space.paneCount(), std::size_t(2));
        // Close: gone.
        r.view.key({'w', false, true, true}, 3);
        a = r.view.answerClose(CloseAnswer::Close);
        EXPECT(a.changed);
        EXPECT_EQ(r.space.paneCount(), std::size_t(1));
        // Answering with nothing pending does nothing.
        EXPECT(!r.view.answerClose(CloseAnswer::Close).changed);
        (void)focused;
    }

    TEST_CASE("Closing a tab asks when any of its panes runs a program, from keys and from the strip") {
        Rig r;
        PaneId other = 0;
        r.view.setBusyProbe([&other](PaneId id) -> std::optional<std::string> {
            return id == other ? std::optional<std::string>("make -j8") : std::nullopt;
        });
        r.split();
        const auto ids = r.space.activeTab()->tree.ids();
        other = ids.front() == r.space.focusedPaneId() ? ids.back() : ids.front(); // not the focused one
        r.view.key({'t', true, true, false}, 0);
        r.view.key({'t', true, true, false}, 0);
        EXPECT_EQ(r.space.tabCount(), std::size_t(3));
        // Select the first tab, close it with the key.
        r.view.key({'1', false, true, true}, 0);
        r.space.select(0);
        KeyResult k = r.view.key({'w', true, true, false}, 1); // CloseTab
        EXPECT(k.confirm.has_value() && k.confirm->scope == CloseScope::Tab);
        EXPECT_EQ(r.space.tabCount(), std::size_t(3));
        // The strip's middle click on that tab asks too.
        r.view.answerClose(CloseAnswer::Cancel);
        MouseResult m = r.view.mouse(r.at(10, 5, MouseInput::Type::Down, MouseInput::Button::Middle));
        EXPECT(m.confirm.has_value() && !m.changed);
        EXPECT_EQ(r.space.tabCount(), std::size_t(3));
        // The tab may have moved by the time the answer comes: it is found by id.
        r.space.moveTab(0, 2);
        KeyResult a = r.view.answerClose(CloseAnswer::Close);
        EXPECT(a.changed);
        EXPECT_EQ(r.space.tabCount(), std::size_t(2));
        EXPECT_EQ(r.space.paneCount(), std::size_t(2)); // The two-pane tab is gone.
    }

    TEST_CASE("Closing the window asks once, naming the programs; no probe asks nothing") {
        Rig r;
        EXPECT(!r.view.windowClose().has_value());
        std::set<PaneId> busy;
        r.view.setBusyProbe([&busy](PaneId id) -> std::optional<std::string> {
            return busy.count(id) ? std::optional<std::string>("ssh host") : std::nullopt;
        });
        EXPECT(!r.view.windowClose().has_value());
        r.view.key({'t', true, true, false}, 0);
        for (const PaneId id : {PaneId(1), PaneId(2)}) busy.insert(id);
        const auto req = r.view.windowClose();
        EXPECT(req.has_value() && req->scope == CloseScope::Window);
        EXPECT_EQ(req->running.size(), std::size_t(2));
        EXPECT(!req->backgroundAvailable);
        EXPECT(req->question().find("2") == std::string::npos); // Both are named, no "more".
        EXPECT(req->question().find("are still running") != std::string::npos);
        // Closing the window changes no layout.
        EXPECT(!r.view.answerClose(CloseAnswer::Close).changed);
        EXPECT_EQ(r.space.tabCount(), std::size_t(2));
    }

    TEST_CASE("The question stays one printable line however hostile the command") {
        CloseRequest q;
        q.scope = CloseScope::Tab;
        q.running.push_back({1, std::string("evil\x1b[2J\nname") + std::string(300, 'x')});
        for (int i = 0; i < 6; ++i) q.running.push_back({PaneId(2 + i), "p" + std::to_string(i)});
        const auto text = q.question();
        for (const char c : text) EXPECT(static_cast<unsigned char>(c) >= 0x20 && c != 0x7f);
        EXPECT(text.find("and 4 more") != std::string::npos);
        EXPECT(text.find("...") != std::string::npos);
        CloseRequest empty;
        empty.running.push_back({1, ""});
        EXPECT(empty.question().find("A program") != std::string::npos);
    }

    TEST_CASE("Closing the last pane of the last tab says so") {
    Rig r;
    const KeyResult k = r.view.key({'w', true, true, false}, 0);
    EXPECT(k.empty && k.changed);
    EXPECT_EQ(r.space.tabCount(), std::size_t(0));
    EXPECT(r.view.plan().panes.empty());
    EXPECT(r.view.sync(0) == false);
}

TEST_CASE("The strip: click selects, close closes, plus opens, wheel switches") {
    Rig r;
    r.view.key({'t', true, true, false}, 0);
    r.view.key({'t', true, true, false}, 0);
    EXPECT_EQ(r.space.tabCount(), std::size_t(3));
    EXPECT_EQ(r.space.activeIndex(), std::size_t(2));
    FramePlan p = r.view.plan();
    // Find the first tab's region and click it.
    int firstTabCol = -1, newCol = -1, closeCol = -1;
    for (const auto &reg : p.strip.regions) {
        if (reg.hit.kind == StripHit::Kind::Tab && reg.hit.tab == 0) firstTabCol = reg.begin;
        if (reg.hit.kind == StripHit::Kind::NewTab) newCol = reg.begin;
        if (reg.hit.kind == StripHit::Kind::Close && reg.hit.tab == 2) closeCol = reg.begin;
    }
    EXPECT(firstTabCol >= 0 && newCol >= 0 && closeCol >= 0);
    MouseResult m = r.view.mouse(r.at(firstTabCol * 8 + 2, 5));
    EXPECT(m.consumed && m.changed && !m.pane);
    EXPECT_EQ(r.space.activeIndex(), std::size_t(0));
    // The wheel over the strip: down goes to the next tab, up to the previous.
    WheelResult w = r.view.wheel(50, 4, -1);
    EXPECT(w.consumed && w.changed);
    EXPECT_EQ(r.space.activeIndex(), std::size_t(1));
    w = r.view.wheel(50, 4, 1);
    EXPECT_EQ(r.space.activeIndex(), std::size_t(0));
    // New tab button.
    m = r.view.mouse(r.at(newCol * 8, 5));
    EXPECT(m.changed);
    EXPECT_EQ(r.space.tabCount(), std::size_t(4));
    // Close button of a tab.
    p = r.view.plan();
    for (const auto &reg : p.strip.regions) {
        if (reg.hit.kind == StripHit::Kind::Close && reg.hit.tab == 3) closeCol = reg.begin;
    }
    m = r.view.mouse(r.at(closeCol * 8, 5));
    EXPECT(m.changed);
    EXPECT_EQ(r.space.tabCount(), std::size_t(3));
    // Hover and release over the strip are consumed without effect.
    m = r.view.mouse(r.at(firstTabCol * 8, 5, MouseInput::Type::Move, MouseInput::Button::None));
    EXPECT(m.consumed && !m.changed);
    // Clicks on an empty part of the strip do nothing.
    m = r.view.mouse(r.at(1199, 5));
    EXPECT(m.consumed);
    EXPECT_EQ(r.space.tabCount(), std::size_t(3));
}

TEST_CASE("Dragging a divider resizes, Shift snaps, the programs follow, sizes are throttled") {
    Rig r;
    r.split();
    r.view.sync(0);
    FramePlan p = r.view.plan();
    const PixelRect gutter = p.dividers[0].rect;
    const int y = gutter.y + 30;
    // Hover shows the resize cursor.
    MouseResult m = r.view.mouse(r.at(gutter.x + 8, y, MouseInput::Type::Move, MouseInput::Button::None));
    EXPECT(m.consumed && m.cursor == MouseResult::Cursor::ResizeColumns && !r.view.dragging());
    // Press in the gutter's outer half (the inner half is the scrollbar's, if shown).
    m = r.view.mouse(r.at(gutter.x + 8, y));
    EXPECT(m.consumed);
    EXPECT(r.view.dragging());
    // Drag left to x = 301 with Shift: whole cells.
    m = r.view.mouse(r.at(301, y, MouseInput::Type::Move, MouseInput::Button::None, true), 100);
    EXPECT(m.consumed && m.changed);
    p = r.view.plan();
    EXPECT_EQ(p.panes[0].rect.w % 8, 0);
    EXPECT(std::abs(p.panes[0].rect.w - (301 - 6)) <= 8);
    // First sync during the drag applies; the next within the interval does not.
    EXPECT(r.view.sync(100));
    r.view.mouse(r.at(400, y, MouseInput::Type::Move), 110);
    const std::size_t before = r.space.pane(p.panes[0].id)->view().model().columns();
    EXPECT(!r.view.sync(110));
    EXPECT_EQ(r.space.pane(p.panes[0].id)->view().model().columns(), before);
    EXPECT(r.view.nextWakeMs().has_value());
    EXPECT(r.view.sync(100 + 40));
    // More motion, throttled again, but the release flushes the last size.
    r.view.mouse(r.at(500, y, MouseInput::Type::Move), 141);
    EXPECT(!r.view.sync(141));
    m = r.view.mouse(r.at(500, y, MouseInput::Type::Up), 142);
    EXPECT(m.consumed);
    EXPECT(!r.view.dragging());
    EXPECT(r.view.sync(143));
    p = r.view.plan();
    EXPECT_EQ(r.space.pane(p.panes[0].id)->view().model().columns(), std::size_t(p.panes[0].columns));
    EXPECT(!r.view.nextWakeMs().has_value());
    // Not dragging: sizes follow at once (no throttle).
    r.view.setWindow({1000, 800, 8, 16});
    EXPECT(r.view.sync(144));
}

TEST_CASE("A drag past the limits keeps the minimum size") {
    Rig r;
    r.split();
    FramePlan p = r.view.plan();
    const PixelRect g = p.dividers[0].rect;
    r.view.mouse(r.at(g.x + 8, g.y + 20));
    r.view.mouse(r.at(-300, 100, MouseInput::Type::Move));
    p = r.view.plan();
    EXPECT(p.panes[0].columns >= 20);
    r.view.mouse(r.at(99999, 100, MouseInput::Type::Move));
    p = r.view.plan();
    EXPECT(p.panes[1].columns >= 20);
    r.view.mouse(r.at(99999, 100, MouseInput::Type::Up));
    EXPECT(!r.view.dragging());
}

TEST_CASE("A press in a pane focuses it and goes to the program with its cell; the drag stays with that pane") {
    Rig r;
    r.split(); // focus on the right pane
    FramePlan p = r.view.plan();
    const PanePlan left = p.panes[0];
    EXPECT(p.panes[1].focused);
    // Press at the third column, second row of the left pane.
    MouseResult m = r.view.mouse(r.at(left.rect.x + 8 * 2 + 3, left.rect.y + 16 + 4));
    EXPECT(m.pane.has_value());
    EXPECT(!m.consumed);
    EXPECT_EQ(m.pane->pane, left.id);
    EXPECT(m.pane->focusedByThis);
    EXPECT(m.pane->cell.has_value());
    EXPECT_EQ(m.pane->cell->column, std::size_t(2));
    EXPECT_EQ(r.space.focusedPaneId(), left.id);
    const FocusChange fc = r.view.takeFocusChange();
    EXPECT_EQ(fc.gained, left.id);
    EXPECT_EQ(r.view.takeFocusChange().gained, PaneId(0)); // reported once
    // Moving into the other pane keeps going to the first one, with a clamped cell.
    m = r.view.mouse(r.at(p.panes[1].rect.x + 50, left.rect.y + 40, MouseInput::Type::Move));
    EXPECT(m.pane.has_value() && m.pane->pane == left.id);
    EXPECT(!m.pane->cell.has_value());
    EXPECT_EQ(m.pane->clamped.column, std::size_t(left.columns - 1));
    EXPECT(!m.pane->focusedByThis);
    m = r.view.mouse(r.at(p.panes[1].rect.x + 50, left.rect.y + 40, MouseInput::Type::Up));
    EXPECT(m.pane.has_value() && m.pane->pane == left.id);
    // After the release a move goes to the pane under the pointer.
    m = r.view.mouse(r.at(p.panes[1].rect.x + 50, left.rect.y + 40, MouseInput::Type::Move));
    EXPECT(m.pane.has_value() && m.pane->pane == p.panes[1].id);
    // A press on the strip never reaches a pane.
    EXPECT(!r.view.mouse(r.at(10, 3)).pane.has_value());
    // Outside every pane (none: the window is covered) nothing is forwarded below the window.
    EXPECT(!r.view.mouse(r.at(10, 5000)).pane.has_value());
}

TEST_CASE("The wheel goes to the pane under the pointer, not the focused one") {
    Rig r;
    r.split();
    const FramePlan p = r.view.plan();
    WheelResult w = r.view.wheel(p.panes[0].rect.x + 20, p.panes[0].rect.y + 20, 1);
    EXPECT(!w.consumed && w.pane.has_value() && *w.pane == p.panes[0].id);
    w = r.view.wheel(p.panes[1].rect.x + 20, p.panes[1].rect.y + 20, -1);
    EXPECT(w.pane.has_value() && *w.pane == p.panes[1].id);
    w = r.view.wheel(p.dividers[0].rect.x + 1, p.panes[0].rect.y + 20, 1);
    EXPECT(!w.pane.has_value());
}

TEST_CASE("The scrollbar of a pane scrolls it when dragged") {
    Rig r;
    r.split();
    FramePlan p = r.view.plan();
    Pane *left = r.space.pane(p.panes[0].id);
    std::string lines;
    for (int i = 0; i < 400; ++i) lines += "line " + std::to_string(i) + "\r\n";
    left->link.push(lines);
    TerminalModel &model = left->view().model();
    EXPECT(model.frame(16).scrollbar.visible);
    const float atBottom = model.frame(16).scrollbar.position;
    EXPECT(atBottom > 0.9f);
    const PixelRect track = p.panes[0].scrollbar;
    // Press at the top of the track: the thumb moves up.
    MouseResult m = r.view.mouse(r.at(track.x + 2, track.y + 2));
    EXPECT(m.consumed);
    EXPECT(r.view.dragging());
    for (int i = 0; i < 20; ++i) model.advanceViewport(0.1);
    const float after = model.frame(16).scrollbar.position;
    EXPECT(after < 0.2f);
    // Drag to the bottom again.
    r.view.mouse(r.at(track.x + 2, track.y + track.h + 50, MouseInput::Type::Move));
    for (int i = 0; i < 20; ++i) model.advanceViewport(0.1);
    EXPECT(model.frame(16).scrollbar.position > 0.9f);
    r.view.mouse(r.at(track.x + 2, track.y + track.h, MouseInput::Type::Up));
    EXPECT(!r.view.dragging());
    // An edge pane's strip works too (right pane, inside its rect).
    Pane *right = r.space.pane(p.panes[1].id);
    right->link.push(lines);
    const PixelRect rr = p.panes[1].rect;
    m = r.view.mouse(r.at(rr.x + rr.w - 3, rr.y + 2));
    EXPECT(m.consumed && r.view.dragging());
    r.view.mouse(r.at(rr.x + rr.w - 3, rr.y + 2, MouseInput::Type::Up));
}

TEST_CASE("Without a scrollbar the gutter half is part of the divider") {
    Rig r;
    r.split();
    const FramePlan p = r.view.plan();
    // Nothing scrolled yet: the left pane has no scrollbar, so the inner half drags the divider.
    const MouseResult m = r.view.mouse(r.at(p.panes[0].scrollbar.x + 1, p.panes[0].rect.y + 30));
    EXPECT(m.consumed);
    EXPECT(r.view.dragging());
    r.view.mouse(r.at(0, 0, MouseInput::Type::Up));
}

TEST_CASE("Hidden tabs keep their panes; the plan covers only the active tab") {
    Rig r;
    r.split();
    r.view.key({'t', true, true, false}, 0); // second tab
    EXPECT_EQ(r.view.plan().panes.size(), std::size_t(1));
    r.view.key({keys::PageUp, true, false, false}, 0); // back to the first
    EXPECT_EQ(r.view.plan().panes.size(), std::size_t(2));
    EXPECT_EQ(r.space.paneCount(), std::size_t(3));
    const FocusChange fc = r.view.takeFocusChange();
    EXPECT(fc.gained != 0);
}

TEST_CASE("Random keys, clicks and window sizes keep the plan consistent") {
    Rig r(900, 600);
    unsigned s = 12345;
    auto next = [&s](unsigned n) {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) % n;
    };
    const KeyChord pool[] = {{'t', true, true, false}, {'e', true, true, false}, {'o', true, true, false},
                             {'w', true, true, false}, {'w', false, true, true}, kPrefix,
                             {'c', false, false, false}, {'%', false, true, false}, {'x', false, false, false},
                             {keys::Left, false, true, true}, {keys::Right, false, true, true},
                             {'=', false, false, false}};
    for (int i = 0; i < 3000; ++i) {
        const auto now = std::int64_t(i) * 20;
        switch (next(4)) {
        case 0: {
            const KeyResult k = r.view.key(pool[next(12)], now);
            if (k.empty) r.view.key({'t', true, true, false}, now);
            break;
        }
        case 1:
            r.view.mouse(r.at(int(next(1000)), int(next(700)), static_cast<MouseInput::Type>(next(3)),
                              static_cast<MouseInput::Button>(1 + next(3)), next(2) == 0),
                         now);
            break;
        case 2:
            r.view.setWindow({int(100 + next(2400)), int(60 + next(1200)), 8, 16});
            break;
        default:
            r.view.tick(now);
            r.view.sync(now);
            break;
        }
        r.view.mouse(r.at(0, 0, MouseInput::Type::Up), now);
        if (r.space.tabCount() == 0) r.view.key({'t', true, true, false}, now);
        const FramePlan p = r.view.plan();
        EXPECT(!p.panes.empty());
        for (const auto &pp : p.panes) {
            EXPECT(pp.columns >= 1 && pp.rows >= 1);
            EXPECT(pp.rect.x >= 0 && pp.rect.y >= p.stripRect.h);
        }
        EXPECT_EQ(p.dividers.size(), p.panes.size() - 1);
        EXPECT(r.space.activeTab()->tree.contains(p.focus));
    }
}

TEST_CASE("A narrow window shows only the focused pane, a wide one all of them") {
    int made = 0;
    Space space{[&made]() { return std::make_unique<Pane>("t" + std::to_string(++made)); }};
    ViewConfig config;
    config.compactBelowColumns = 200; // 1200 px / 8 = 150 columns: compact
    View view(space, config);
    view.setWindow({1200, 800, 8, 16});
    view.key({'t', true, true, false}, 0);
    view.key({'e', true, true, false}, 1); // two panes, the new one focused
    view.sync(2);
    FramePlan plan = view.plan();
    EXPECT_EQ(plan.panes.size(), std::size_t(1));
    EXPECT(plan.dividers.empty());
    EXPECT_EQ(plan.panes[0].id, space.focusedPaneId());
    EXPECT_EQ(plan.panes[0].rect.w, 1200);
    // The focus moves with the prefix keys and the other pane shows.
    const PaneId before = space.focusedPaneId();
    view.key(kPrefix, 10);
    view.key({keys::Left, false, false, false}, 11);
    EXPECT(space.focusedPaneId() != before);
    EXPECT_EQ(view.plan().panes[0].id, space.focusedPaneId());
    // A click goes to the shown pane.
    MouseInput m;
    m.x = 100;
    m.y = 100;
    m.type = MouseInput::Type::Down;
    m.button = MouseInput::Button::Left;
    const MouseResult down = view.mouse(m);
    EXPECT(down.pane && down.pane->pane == space.focusedPaneId());
    // Wide enough: both panes again.
    View wide(space, ViewConfig{});
    wide.setWindow({1200, 800, 8, 16});
    EXPECT_EQ(wide.plan().panes.size(), std::size_t(2));
}

TEST_CASE("A finger grabs a divider from a distance and the strip from below") {
    Rig r;
    r.split();
    const FramePlan plan = r.view.plan();
    EXPECT_EQ(plan.dividers.size(), std::size_t(1));
    const PixelRect g = plan.dividers[0].rect;
    const int near = g.x + g.w + 10; // 10 px outside the gutter, inside the pane
    MouseInput mouse = r.at(near, 300);
    EXPECT(!r.view.mouse(mouse).consumed || r.view.dragging()); // A mouse hits the pane.
    r.view.mouse(r.at(near, 300, MouseInput::Type::Up));
    MouseInput touch = r.at(near, 300);
    touch.touch = true;
    MouseResult down = r.view.mouse(touch);
    EXPECT(down.consumed);
    EXPECT(down.cursor == MouseResult::Cursor::ResizeColumns);
    EXPECT(r.view.dragging());
    touch.type = MouseInput::Type::Move;
    touch.x = near + 60;
    EXPECT(r.view.mouse(touch).changed);
    touch.type = MouseInput::Type::Up;
    r.view.mouse(touch);
    EXPECT(!r.view.dragging());
    EXPECT(r.view.plan().panes[0].rect.w > plan.panes[0].rect.w + 40);

    // The strip: a tap just below the row still counts for a finger.
    Rig two;
    two.view.key({'t', true, true, false}, 5); // second tab: the strip shows
    const FramePlan withStrip = two.view.plan();
    EXPECT(withStrip.stripRect.h > 0);
    const int below = withStrip.stripRect.h + 8;
    MouseInput tap = two.at(8, below);
    EXPECT(!two.view.mouse(tap).consumed);
    two.view.mouse(two.at(8, below, MouseInput::Type::Up));
    tap.touch = true;
    EXPECT(two.view.mouse(tap).consumed);
}

TEST_CASE("The prefix colon and the desktop shortcut ask for the palette") {
    Rig r;
    r.view.key(kPrefix, 0);
    KeyResult k = r.view.key({':', false, false, false}, 1);
    EXPECT(k.openPalette && !k.forwardKey && !k.forwardPrefix);
    EXPECT_EQ(r.space.tabCount(), std::size_t(1));
    k = r.view.key({'p', true, true, false}, 2);
    EXPECT(k.openPalette);
}

TEST_CASE("A layer takes keys first, is drawn over the panes and closes on Escape") {
    Rig r;
    r.view.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry()));
    EXPECT(r.view.hasLayer());
    FramePlan plan = r.view.plan();
    EXPECT_EQ(plan.layers.size(), std::size_t(1));
    const PixelRect box = plan.layers[0].rect;
    EXPECT(box.x >= 0 && box.y >= 0 && box.x + box.w <= 1200 && box.y + box.h <= 800);
    EXPECT(box.x % 8 == 0 && box.y % 16 == 0 && box.w % 8 == 0 && box.h % 16 == 0);
    EXPECT(plan.layers[0].cells != nullptr);
    EXPECT_EQ(plan.layers[0].cells->columns() * 8, std::size_t(box.w));
    EXPECT(plan.layers[0].cursor.visible);
    // Typing goes to the layer, not the program.
    const KeyResult typed = r.view.key({'n', false, false, false}, 0);
    EXPECT(typed.changed && !typed.forwardKey && !typed.forwardPrefix);
    EXPECT(r.view.key(kPrefix, 1).commands.empty()); // Not the prefix either.
    EXPECT(r.view.hasLayer());
    const KeyResult esc = r.view.key({keys::Escape, false, false, false}, 2);
    EXPECT(esc.changed);
    EXPECT(!r.view.hasLayer());
    EXPECT(r.view.plan().layers.empty());
    EXPECT(r.view.key({'a', false, false, false}, 3).forwardKey); // Normal again.
}

TEST_CASE("Enter in the palette runs the command and closes it") {
    Rig r;
    r.view.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry()));
    for (const char c : std::string("new tab")) r.view.key({c, false, false, false}, 0);
    const KeyResult k = r.view.key({keys::Enter, false, false, false}, 1);
    EXPECT(!r.view.hasLayer());
    EXPECT_EQ(r.space.tabCount(), std::size_t(2));
    EXPECT_EQ(k.commands.size(), std::size_t(1));
    // The palette entry of the palette asks the owner to open it again.
    r.view.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry()));
    for (const char c : std::string("command palette")) r.view.key({c, false, false, false}, 2);
    EXPECT(r.view.key({keys::Enter, false, false, false}, 3).openPalette);
}

TEST_CASE("Mouse: a click on an entry runs it, outside closes, the rest is swallowed") {
    Rig r;
    r.view.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry()));
    for (const char c : std::string("new tab")) r.view.key({c, false, false, false}, 0);
    const PixelRect box = r.view.plan().layers[0].rect;
    MouseInput move = r.at(box.x + 20, box.y + 40, MouseInput::Type::Move, MouseInput::Button::None);
    EXPECT(r.view.mouse(move).consumed);
    EXPECT(r.view.mouse(r.at(box.x + 20, box.y + 40, MouseInput::Type::Up)).consumed);
    EXPECT(r.view.wheel(box.x + 20, box.y + 40, -1).consumed);
    EXPECT(r.view.hasLayer());
    // Second text row of the layer = first entry.
    const MouseResult hit = r.view.mouse(r.at(box.x + 8 * 4, box.y + 2 * 16 + 4));
    EXPECT(hit.consumed && hit.changed);
    EXPECT(!r.view.hasLayer());
    EXPECT_EQ(r.space.tabCount(), std::size_t(2));
    r.view.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry()));
    const MouseResult outside = r.view.mouse(r.at(1190, 790));
    EXPECT(outside.consumed && outside.changed);
    EXPECT(!r.view.hasLayer());
    EXPECT_EQ(r.space.tabCount(), std::size_t(2)); // Nothing ran.
    EXPECT(r.view.mouse(r.at(500, 400, MouseInput::Type::Up)).pane.has_value() ||
           !r.view.hasLayer()); // Mouse works for the panes again.
}

TEST_CASE("The strip can end in a button that asks for the palette") {
    int made = 0;
    Space space{[&made]() { return std::make_unique<Pane>("t" + std::to_string(++made)); }};
    ViewConfig config;
    config.paletteButton = true;
    View view(space, config);
    view.setWindow({1200, 800, 8, 16});
    view.key({'t', true, true, false}, 0);
    view.key({'t', true, true, false}, 1); // Two tabs: the strip shows.
    const FramePlan plan = view.plan();
    EXPECT_NE(plan.stripRect.h, 0);
    EXPECT_NE(plan.strip.regions.size(), std::size_t(0));
    EXPECT(plan.strip.regions.back().hit.kind == StripHit::Kind::Palette);
    const int last = int(plan.strip.cells.size()) - 1;
    EXPECT(plan.strip.cells[std::size_t(last - 1)].codePoint == U'\x2261');
    MouseInput click;
    click.x = last * 8 + 2;
    click.y = 4;
    click.type = MouseInput::Type::Down;
    click.button = MouseInput::Button::Left;
    const MouseResult r = view.mouse(click);
    EXPECT(r.consumed && r.openPalette);
    // Without the option there is no button.
    View plain(space, ViewConfig{});
    plain.setWindow({1200, 800, 8, 16});
    EXPECT(plain.plan().strip.regions.empty() ||
           plain.plan().strip.regions.back().hit.kind != StripHit::Kind::Palette);
}

TEST_CASE("The Share button shows the shared panes of the active tab and asks for the right action") {
    int made = 0;
    Space space{[&made]() { return std::make_unique<Pane>("t" + std::to_string(++made)); }};
    ViewConfig config;
    config.shareButton = true;
    View view(space, config);
    view.setWindow({1200, 800, 8, 16});
    view.key({'t', true, true, false}, 0);
    view.key({'t', true, true, false}, 1); // Two tabs: the strip shows.
    // No probe (panes not in the daemon): no button.
    for (const auto &region : view.plan().strip.regions) {
        EXPECT(region.hit.kind != StripHit::Kind::Share);
    }
    std::set<PaneId> shared;
    view.setShareProbe([&shared](PaneId id) { return shared.count(id) != 0; });
    const auto clickShare = [&view]() {
        const FramePlan plan = view.plan();
        int column = -1;
        for (const auto &region : plan.strip.regions) {
            if (region.hit.kind == StripHit::Kind::Share) column = region.begin;
        }
        if (column < 0) {
            return MouseResult{}; // The caller's check of the action fails.
        }
        MouseInput click;
        click.x = column * 8 + 2;
        click.y = 4;
        click.type = MouseInput::Type::Down;
        click.button = MouseInput::Button::Left;
        return view.mouse(click);
    };
    EXPECT_EQ(clickShare().action, "share.tab");
    const PaneId pane = space.focusedPaneId();
    shared.insert(pane);
    EXPECT_EQ(view.sharedInActiveTab(), 1);
    EXPECT_EQ(clickShare().action, "share.tab.stop");
    bool marked = false;
    for (const auto &p : view.plan().panes) {
        EXPECT(p.shared == (p.id == pane));
        marked = marked || p.shared;
    }
    EXPECT(marked);
}

TEST_SUIT_END
