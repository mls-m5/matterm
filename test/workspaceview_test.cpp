#include "mls-unit-test/unittest.h"
#include "workspaceview.h"

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

TEST_SUIT_END
