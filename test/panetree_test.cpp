#include "mls-unit-test/unittest.h"
#include "panetree.h"

#include <cstdint>
#include <memory>
#include <string>
#include <random>
#include <set>
#include <vector>

using namespace frontend;

namespace {

// A pane that counts how many exist, to prove nothing leaks or dies twice.
struct Fake {
    static int alive;
    static int created;
    int tag;
    explicit Fake(int t = 0) : tag(t) {
        ++alive;
        ++created;
    }
    Fake(const Fake &) = delete;
    Fake &operator=(const Fake &) = delete;
    ~Fake() { --alive; }
};
int Fake::alive = 0;
int Fake::created = 0;

using Tree = PaneTree<Fake>;

std::unique_ptr<Fake> make(int tag = 0) {
    return std::make_unique<Fake>(tag);
}

LayoutSpec spec(int w, int h, int cw = 8, int ch = 16, int gutter = 6) {
    LayoutSpec s;
    s.area = {0, 0, w, h};
    s.cellWidth = cw;
    s.cellHeight = ch;
    s.dividerThickness = gutter;
    return s;
}

PaneId nextId = 1000;
SplitResult splitOf(Tree &t, PaneId target, SplitDirection d, const LayoutSpec &s, PaneId *made = nullptr) {
    auto p = make();
    const PaneId id = ++nextId;
    const SplitResult r = t.split(target, d, id, p, s);
    if (made != nullptr) *made = r == SplitResult::Ok ? id : 0;
    if (r != SplitResult::Ok) {
        // The caller must still own the pane.
        if (!p) return SplitResult::BadArgument;
    }
    return r;
}

// The boxes and gutters lie inside the area, do not overlap, and their areas add up to
// the whole area: so every pixel is covered exactly once. Empty string if so.
std::string tilingProblem(const Layout &l, const PixelRect &area) {
    std::vector<PixelRect> rects;
    for (const auto &p : l.panes) rects.push_back(p.rect);
    for (const auto &d : l.dividers) rects.push_back(d.rect);
    long long sum = 0;
    for (const auto &r : rects) {
        if (r.w < 0 || r.h < 0) return "negative size";
        if (r.x < area.x || r.y < area.y || r.x + r.w > area.x + area.w || r.y + r.h > area.y + area.h) {
            return "outside the area";
        }
        sum += static_cast<long long>(r.w) * r.h;
    }
    for (std::size_t i = 0; i < rects.size(); ++i) {
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
            const auto &a = rects[i];
            const auto &b = rects[j];
            const bool apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
            if (!apart && a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0) return "overlap";
        }
    }
    if (sum != static_cast<long long>(std::max(area.w, 0)) * std::max(area.h, 0)) return "areas do not add up";
    return "";
}

// Each pane's cells and leftovers account for its box.
bool cellsConsistent(const Layout &l, const LayoutSpec &s) {
    for (const auto &p : l.panes) {
        const int usedW = p.columns * s.cellWidth;
        const int usedH = p.rows * s.cellHeight;
        if (usedW > p.rect.w || p.rect.w - usedW >= s.cellWidth) return false;
        if (usedH > p.rect.h || p.rect.h - usedH >= s.cellHeight) return false;
        if (p.leftover[0].w != p.rect.w - usedW || p.leftover[0].h != p.rect.h) return false;
        if (p.leftover[1].h != p.rect.h - usedH || p.leftover[1].w != usedW) return false;
    }
    return true;
}

} // namespace

TEST_SUIT_BEGIN(PaneTree)

TEST_CASE("One pane fills the area and counts whole cells and leftover") {
    Tree t;
    EXPECT(t.setRoot(1, make()));
    const LayoutSpec s = spec(103, 70, 8, 16, 6);
    const Layout l = t.layout(s);
    EXPECT_EQ(l.panes.size(), std::size_t(1));
    EXPECT(l.dividers.empty());
    const PaneBox &b = l.panes[0];
    EXPECT_EQ(b.pane, PaneId(1));
    EXPECT(b.rect == (PixelRect{0, 0, 103, 70}));
    EXPECT_EQ(b.columns, 12);
    EXPECT_EQ(b.rows, 4);
    EXPECT(b.leftover[0] == (PixelRect{96, 0, 7, 70}));
    EXPECT(b.leftover[1] == (PixelRect{0, 64, 96, 6}));
}

TEST_CASE("Two panes side by side share the area with a gutter") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(400, 160);
    PaneId two = 0;
    EXPECT(splitOf(t, 1, SplitDirection::SideBySide, s, &two) == SplitResult::Ok);
    EXPECT_EQ(t.size(), std::size_t(2));
    EXPECT_EQ(t.focus(), two);
    const Layout l = t.layout(s);
    EXPECT_EQ(l.panes.size(), std::size_t(2));
    EXPECT_EQ(l.dividers.size(), std::size_t(1));
    EXPECT_EQ(l.panes[0].rect.w + 6 + l.panes[1].rect.w, 400);
    EXPECT_EQ(l.dividers[0].rect.w, 6);
    EXPECT_EQ(l.dividers[0].rect.h, 160);
    EXPECT(l.dividers[0].direction == SplitDirection::SideBySide);
    EXPECT_EQ(l.dividers[0].before, PaneId(1));
    EXPECT_EQ(l.dividers[0].after, two);
    EXPECT_EQ(tilingProblem(l, s.area), std::string());
}

TEST_CASE("Tiling is exact for many sizes, cells and trees") {
    std::mt19937 rng(20261121);
    for (int round = 0; round < 300; ++round) {
        const int cw = 1 + int(rng() % 14);
        const int ch = 1 + int(rng() % 30);
        const int gutter = int(rng() % 10);
        // Big enough for the tree below, small enough to keep the painting fast.
        const int w = 120 * cw + int(rng() % 400);
        const int h = 40 * ch + int(rng() % 300);
        LayoutSpec s = spec(w, h, cw, ch, gutter);
        s.area.x = int(rng() % 50);
        s.area.y = int(rng() % 50);
        Tree t;
        t.setRoot(++nextId, make());
        for (int i = 0; i < 12; ++i) {
            const auto ids = t.ids();
            const PaneId target = ids[rng() % ids.size()];
            splitOf(t, target, rng() % 2 ? SplitDirection::Stacked : SplitDirection::SideBySide, s);
            if (rng() % 3 == 0) {
                // Move a divider somewhere odd.
                const Layout l = t.layout(s);
                if (!l.dividers.empty()) {
                    const std::size_t k = rng() % l.dividers.size();
                    t.setRatio(l.dividerSplits[k], 0.05 + double(rng() % 90) / 100.0);
                }
            }
        }
        const Layout l = t.layout(s);
        const std::string problem = tilingProblem(l, s.area);
        EXPECT_EQ(problem, std::string());
        EXPECT(cellsConsistent(l, s));
        EXPECT_EQ(l.panes.size(), t.size());
        EXPECT_EQ(l.dividers.size(), t.size() - 1);
        EXPECT_EQ(l.dividerSplits.size(), l.dividers.size());
        for (const auto &p : l.panes) {
            EXPECT(p.columns >= s.minColumns);
            EXPECT(p.rows >= s.minRows);
        }
    }
}

TEST_CASE("The minimum size is kept under extreme ratios") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(400, 200, 8, 16, 4);
    PaneId two = 0, three = 0;
    EXPECT(splitOf(t, 1, SplitDirection::SideBySide, s, &two) == SplitResult::Ok);
    EXPECT(splitOf(t, two, SplitDirection::Stacked, s, &three) == SplitResult::Ok);
    Layout l = t.layout(s);
    for (double r : {0.0001, 0.001, 0.5, 0.999, 0.9999}) {
        for (SplitId id : l.dividerSplits) t.setRatio(id, r);
        const Layout m = t.layout(s);
        EXPECT_EQ(tilingProblem(m, s.area), std::string());
        for (const auto &p : m.panes) {
            EXPECT(p.columns >= 20);
            EXPECT(p.rows >= 5);
        }
    }
    EXPECT(!t.setRatio(l.dividerSplits[0], 0.0));
    EXPECT(!t.setRatio(l.dividerSplits[0], 1.0));
    EXPECT(!t.setRatio(9999, 0.5));
}

TEST_CASE("A split is refused when two minimum panes would not fit") {
    Tree t;
    t.setRoot(1, make());
    // Exactly two minimum columns and a gutter: 20*8*2 + 6 = 326.
    const LayoutSpec fits = spec(326, 80);
    EXPECT(t.canSplit(1, SplitDirection::SideBySide, fits) == SplitResult::Ok);
    const LayoutSpec tight = spec(325, 80);
    EXPECT(t.canSplit(1, SplitDirection::SideBySide, tight) == SplitResult::TooSmall);
    // Rows: 2*5*16 + 6 = 166.
    EXPECT(t.canSplit(1, SplitDirection::Stacked, spec(400, 166)) == SplitResult::Ok);
    EXPECT(t.canSplit(1, SplitDirection::Stacked, spec(400, 165)) == SplitResult::TooSmall);
    const int before = Fake::alive;
    auto p = make();
    EXPECT(t.split(1, SplitDirection::SideBySide, 2, p, tight) == SplitResult::TooSmall);
    EXPECT(p != nullptr); // the caller still owns it
    EXPECT_EQ(t.size(), std::size_t(1));
    EXPECT_EQ(Fake::alive, before + 1);
    p.reset();
}

TEST_CASE("Bad arguments are refused without taking the pane") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(800, 400);
    auto p = make();
    EXPECT(t.split(99, SplitDirection::SideBySide, 2, p, s) == SplitResult::BadArgument); // no such pane
    EXPECT(t.split(1, SplitDirection::SideBySide, 0, p, s) == SplitResult::BadArgument);  // id 0
    EXPECT(t.split(1, SplitDirection::SideBySide, 1, p, s) == SplitResult::BadArgument);  // id in use
    std::unique_ptr<Fake> none;
    EXPECT(t.split(1, SplitDirection::SideBySide, 2, none, s) == SplitResult::BadArgument);
    EXPECT(p != nullptr);
    EXPECT(!t.setRoot(5, make())); // not empty
    Tree empty;
    EXPECT(!empty.setRoot(0, make()));
    EXPECT(!empty.setRoot(1, nullptr));
    EXPECT(empty.close(1) == nullptr);
    EXPECT(empty.layout(s).panes.empty());
}

TEST_CASE("No more than eight panes in a tab") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(4000, 2000);
    for (int i = 0; i < 7; ++i) {
        EXPECT(splitOf(t, t.focus(), i % 2 ? SplitDirection::Stacked : SplitDirection::SideBySide, s) ==
               SplitResult::Ok);
    }
    EXPECT_EQ(t.size(), kMaxPanesPerTab);
    const int before = Fake::alive;
    EXPECT(splitOf(t, t.focus(), SplitDirection::SideBySide, s) == SplitResult::TooManyPanes);
    EXPECT_EQ(Fake::alive, before);
    EXPECT_EQ(tilingProblem(t.layout(s), s.area), std::string());
}

TEST_CASE("Closing a pane gives the space to its sibling and hands the pane back") {
    Tree t;
    t.setRoot(1, make(11));
    const LayoutSpec s = spec(500, 200);
    PaneId two = 0, three = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &two);
    splitOf(t, two, SplitDirection::Stacked, s, &three);
    // 1 | (2 over 3). Close 2: 3 takes its half.
    t.setFocus(two);
    auto gone = t.close(two);
    EXPECT(gone != nullptr);
    EXPECT_EQ(t.size(), std::size_t(2));
    EXPECT_EQ(t.focus(), three); // the heir has the focus
    const Layout l = t.layout(s);
    EXPECT_EQ(l.panes.size(), std::size_t(2));
    EXPECT_EQ(l.panes[1].rect.h, 200);
    EXPECT_EQ(tilingProblem(l, s.area), std::string());
    // Closing a pane that does not have the focus leaves the focus alone.
    EXPECT(t.close(1) != nullptr);
    EXPECT_EQ(t.focus(), three);
    EXPECT(t.close(1) == nullptr); // already gone
    // The last pane empties the tree.
    EXPECT(t.close(three) != nullptr);
    EXPECT(t.empty());
    EXPECT_EQ(t.focus(), PaneId(0));
}

TEST_CASE("Random split and close sequences keep every invariant and every pane accounted for") {
    const int aliveAtStart = Fake::alive;
    std::mt19937 rng(7);
    for (int round = 0; round < 60; ++round) {
        const LayoutSpec s = spec(300 + int(rng() % 1500), 120 + int(rng() % 900), 8, 16, 4);
        std::set<PaneId> model;
        {
            Tree t;
            std::vector<std::unique_ptr<Fake>> held; // panes handed back by close()
            const PaneId root = ++nextId;
            t.setRoot(root, make());
            model.insert(root);
            for (int step = 0; step < 80; ++step) {
                const auto ids = t.ids();
                const PaneId pick = ids[rng() % ids.size()];
                switch (rng() % 6) {
                case 0:
                case 1:
                case 2: {
                    PaneId made = 0;
                    if (splitOf(t, pick, rng() % 2 ? SplitDirection::Stacked : SplitDirection::SideBySide, s,
                                &made) == SplitResult::Ok) {
                        model.insert(made);
                    }
                    break;
                }
                case 3:
                    if (ids.size() > 1) {
                        auto p = t.close(pick);
                        EXPECT(p != nullptr);
                        model.erase(pick);
                        if (rng() % 2) held.push_back(std::move(p));
                    }
                    break;
                case 4: {
                    const Layout l = t.layout(s);
                    if (!l.dividers.empty()) {
                        const std::size_t k = rng() % l.dividers.size();
                        const int pointer = int(rng() % 2000) - 300;
                        t.dragDivider(l.dividerSplits[k], pointer, rng() % 2 == 0, s);
                    }
                    break;
                }
                default:
                    if (rng() % 4 == 0) t.evenOut(s);
                    t.moveFocus(static_cast<FocusDirection>(rng() % 4), s);
                    break;
                }
                // Invariants after every step.
                EXPECT_EQ(t.size(), model.size());
                const auto now = t.ids();
                EXPECT(std::set<PaneId>(now.begin(), now.end()) == model);
                EXPECT(t.contains(t.focus()));
                const Layout l = t.layout(s);
                EXPECT_EQ(l.panes.size(), model.size());
                EXPECT_EQ(tilingProblem(l, s.area), std::string());
                const auto min = t.minimumSize(s);
                if (s.area.w >= min.first && s.area.h >= min.second) {
                    for (const auto &p : l.panes) {
                        EXPECT(p.columns >= s.minColumns);
                        EXPECT(p.rows >= s.minRows);
                    }
                }
            }
            // Panes in the tree plus those handed back are all that is alive.
            EXPECT_EQ(Fake::alive, aliveAtStart + int(model.size()) + int(held.size()));
        }
        EXPECT_EQ(Fake::alive, aliveAtStart); // tree and held list gone: nothing leaked
    }
}

TEST_CASE("Focus by direction follows geometry on an L-shaped layout") {
    // A on the left, full height; B on top right (30 %); C below right (70 %).
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(700, 500, 8, 16, 6);
    PaneId b = 0, c = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &b);
    splitOf(t, b, SplitDirection::Stacked, s, &c);
    const Layout l = t.layout(s);
    t.setRatio(l.dividerSplits[1], 0.3);
    const PaneId a = 1;

    EXPECT_EQ(t.neighbour(b, FocusDirection::Left, s), a);
    EXPECT_EQ(t.neighbour(c, FocusDirection::Left, s), a);
    EXPECT_EQ(t.neighbour(a, FocusDirection::Right, s), c); // C shares more of A's edge
    EXPECT_EQ(t.neighbour(b, FocusDirection::Down, s), c);
    EXPECT_EQ(t.neighbour(c, FocusDirection::Up, s), b);
    // Nothing beyond the edges.
    EXPECT_EQ(t.neighbour(a, FocusDirection::Left, s), PaneId(0));
    EXPECT_EQ(t.neighbour(a, FocusDirection::Up, s), PaneId(0));
    EXPECT_EQ(t.neighbour(b, FocusDirection::Right, s), PaneId(0));
    EXPECT_EQ(t.neighbour(b, FocusDirection::Up, s), PaneId(0));
    EXPECT_EQ(t.neighbour(c, FocusDirection::Down, s), PaneId(0));
    EXPECT_EQ(t.neighbour(99, FocusDirection::Left, s), PaneId(0));

    t.setFocus(c);
    EXPECT(t.moveFocus(FocusDirection::Left, s));
    EXPECT_EQ(t.focus(), a);
    EXPECT(!t.moveFocus(FocusDirection::Left, s));
    EXPECT_EQ(t.focus(), a);
    EXPECT(t.moveFocus(FocusDirection::Right, s));
    EXPECT_EQ(t.focus(), c);
}

TEST_CASE("Focus follows geometry, not tree order, in a grid") {
    // (1 | 2) over (3 | 4), built so the tree order is not the reading order.
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(900, 600, 8, 16, 6);
    PaneId bottom = 0, topRight = 0, bottomRight = 0;
    splitOf(t, 1, SplitDirection::Stacked, s, &bottom);                     // 1 over bottom
    splitOf(t, 1, SplitDirection::SideBySide, s, &topRight);                // (1|topRight) over bottom
    splitOf(t, bottom, SplitDirection::SideBySide, s, &bottomRight);        // ... over (bottom|bottomRight)
    EXPECT_EQ(t.neighbour(1, FocusDirection::Down, s), bottom);
    EXPECT_EQ(t.neighbour(topRight, FocusDirection::Down, s), bottomRight);
    EXPECT_EQ(t.neighbour(bottomRight, FocusDirection::Up, s), topRight);
    EXPECT_EQ(t.neighbour(bottom, FocusDirection::Right, s), bottomRight);
    EXPECT_EQ(t.neighbour(bottomRight, FocusDirection::Left, s), bottom);
}

TEST_CASE("Dragging a divider follows the pointer, clamps and snaps") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(806, 200, 8, 16, 6); // 800 available
    PaneId two = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &two);
    const SplitId split = t.layout(s).dividerSplits[0];

    // Pointer at x = 303: the divider centre follows (gutter 6 -> starts at 300).
    EXPECT(t.dragDivider(split, 303, false, s));
    EXPECT_EQ(t.layout(s).panes[0].rect.w, 300);
    EXPECT_EQ(t.layout(s).dividers[0].rect.x, 300);
    // Shift: whole cells (8 px): 301..: nearest multiple of 8 to 301-... is 296 or 304.
    EXPECT(t.dragDivider(split, 305, true, s)); // wants 302 -> 304
    EXPECT_EQ(t.layout(s).panes[0].rect.w, 304);
    EXPECT_EQ(t.layout(s).panes[0].columns, 38);
    EXPECT_EQ(t.layout(s).panes[0].leftover[0].w, 0);
    // Far left: clamped to 20 columns (160 px); far right: leaves 160 px.
    EXPECT(t.dragDivider(split, -500, false, s));
    EXPECT_EQ(t.layout(s).panes[0].rect.w, 160);
    EXPECT(t.dragDivider(split, 5000, false, s));
    EXPECT_EQ(t.layout(s).panes[1].rect.w, 160);
    EXPECT_EQ(t.layout(s).panes[0].rect.w, 640);
    // No change: reports false.
    EXPECT(!t.dragDivider(split, 5000, false, s));
    EXPECT(!t.dragDivider(4242, 100, false, s));
    // The ratio survives layout exactly: no drift when asked again.
    t.dragDivider(split, 403, false, s);
    const int w = t.layout(s).panes[0].rect.w;
    for (int i = 0; i < 5; ++i) EXPECT_EQ(t.layout(s).panes[0].rect.w, w);
}

TEST_CASE("Dragging works at every pixel of a nested split, and the ratio is exact") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(1000, 500, 9, 17, 5);
    PaneId two = 0, three = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &two);
    splitOf(t, two, SplitDirection::Stacked, s, &three);
    const Layout l = t.layout(s);
    const SplitId inner = l.dividerSplits[1];
    for (int y = 0; y < 520; ++y) {
        t.dragDivider(inner, y, false, s);
        const Layout m = t.layout(s);
        EXPECT_EQ(tilingProblem(m, s.area), std::string());
        for (const auto &p : m.panes) EXPECT(p.rows >= 5);
    }
}

TEST_CASE("Even out shares by panes along the axis and snaps to cells") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(1000, 600, 8, 16, 6);
    PaneId two = 0, three = 0, four = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &two);
    splitOf(t, two, SplitDirection::SideBySide, s, &three); // 1 | (2 | 3): ratios 1/2, 1/2
    splitOf(t, three, SplitDirection::Stacked, s, &four);
    for (SplitId id : t.layout(s).dividerSplits) t.setRatio(id, 0.9);
    t.evenOut(s);
    const Layout l = t.layout(s);
    EXPECT_EQ(tilingProblem(l, s.area), std::string());
    // Three columns: widths within a cell (and a gutter) of each other.
    const PaneBox *p1 = l.box(1), *p2 = l.box(two), *p3 = l.box(three), *p4 = l.box(four);
    EXPECT(p1 && p2 && p3 && p4);
    const int a = p1->rect.w, b = p2->rect.w, c = p3->rect.w;
    EXPECT(std::abs(a - b) <= 8 + 6);
    EXPECT(std::abs(b - c) <= 8 + 6);
    EXPECT(std::abs(a - c) <= 8 + 6);
    EXPECT_EQ(p3->rect.w, p4->rect.w);
    // Snapped: the first child of every split has whole cells (no leftover).
    EXPECT_EQ(p1->leftover[0].w, 0);
    EXPECT_EQ(p2->leftover[0].w, 0);
    // Stacked 3 over 4: equal rows within one.
    EXPECT(std::abs(p3->rows - p4->rows) <= 1);
}

TEST_CASE("Gutters name the panes on both sides") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec s = spec(900, 600);
    PaneId two = 0, three = 0;
    splitOf(t, 1, SplitDirection::SideBySide, s, &two);
    splitOf(t, 1, SplitDirection::Stacked, s, &three); // (1 over three) | two
    const Layout l = t.layout(s);
    EXPECT_EQ(l.dividers.size(), std::size_t(2));
    for (std::size_t i = 0; i < l.dividers.size(); ++i) {
        const auto &d = l.dividers[i];
        const PaneBox *a = l.box(d.before);
        const PaneBox *b = l.box(d.after);
        EXPECT(a && b);
        if (d.direction == SplitDirection::SideBySide) {
            EXPECT_EQ(a->rect.x + a->rect.w, d.rect.x);
            EXPECT_EQ(d.rect.x + d.rect.w, b->rect.x);
        }
        else {
            EXPECT_EQ(a->rect.y + a->rect.h, d.rect.y);
            EXPECT_EQ(d.rect.y + d.rect.h, b->rect.y);
        }
    }
    // Hit tests.
    EXPECT_EQ(l.paneAt(1, 1), PaneId(1));
    EXPECT_EQ(l.paneAt(899, 599), two);
    const int g = l.dividerAt(l.dividers[0].rect.x + 1, 5);
    EXPECT(g >= 0);
    EXPECT_EQ(l.dividerAt(-1, -1), -1);
}

TEST_CASE("An area below the minimum degrades without negative sizes or overlap") {
    Tree t;
    t.setRoot(1, make());
    const LayoutSpec big = spec(2000, 1000);
    PaneId two = 0, three = 0;
    splitOf(t, 1, SplitDirection::SideBySide, big, &two);
    splitOf(t, two, SplitDirection::Stacked, big, &three);
    for (int w : {0, 1, 5, 50, 200, 330}) {
        for (int h : {0, 1, 7, 90, 150}) {
            const LayoutSpec s = spec(w, h);
            const Layout l = t.layout(s);
            EXPECT_EQ(l.panes.size(), std::size_t(3));
            EXPECT_EQ(tilingProblem(l, s.area), std::string());
            for (const auto &p : l.panes) {
                EXPECT(p.rect.w >= 0 && p.rect.h >= 0);
                EXPECT(p.columns >= 0 && p.rows >= 0);
            }
        }
    }
    const LayoutSpec negative = spec(-5, -5);
    EXPECT_EQ(t.layout(negative).panes.size(), std::size_t(3));
}

TEST_CASE("The tree owns its panes and releases them exactly once") {
    const int before = Fake::alive;
    {
        Tree t;
        const LayoutSpec s = spec(2000, 1000);
        t.setRoot(1, make(1));
        PaneId a = 0, b = 0;
        splitOf(t, 1, SplitDirection::SideBySide, s, &a);
        splitOf(t, a, SplitDirection::Stacked, s, &b);
        EXPECT_EQ(Fake::alive, before + 3);
        // Moving the tree moves the panes with it.
        Tree moved = std::move(t);
        EXPECT_EQ(Fake::alive, before + 3);
        EXPECT_EQ(moved.size(), std::size_t(3));
        // A closed pane survives its tree.
        auto kept = moved.close(a);
        EXPECT(kept != nullptr);
        EXPECT_EQ(Fake::alive, before + 3);
        // pane() gives access without ownership.
        EXPECT(moved.pane(1) != nullptr);
        EXPECT_EQ(moved.pane(1)->tag, 1);
        EXPECT(moved.pane(a) == nullptr);
        kept.reset();
        EXPECT_EQ(Fake::alive, before + 2);
    }
    EXPECT_EQ(Fake::alive, before);
}

TEST_CASE("fold reports the shape, and setParentRatio sets the proportion above a pane") {
    Tree t;
    t.setRoot(1, make(1));
    const LayoutSpec sp = spec(1000, 600);
    auto p2 = make(2);
    EXPECT(t.split(1, SplitDirection::SideBySide, 2, p2, sp) == SplitResult::Ok);
    auto p3 = make(3);
    EXPECT(t.split(2, SplitDirection::Stacked, 3, p3, sp) == SplitResult::Ok);
    const auto shape = [&t] {
        return t.fold<std::string>(
            [](PaneId id, const Fake &) { return std::to_string(id); },
            [](SplitDirection d, double ratio, std::string a, std::string b) {
                return std::string(d == SplitDirection::SideBySide ? "H" : "V") +
                       std::to_string(int(ratio * 1000 + 0.5)) + "(" + a + "," + b + ")";
            });
    };
    EXPECT_EQ(shape(), std::string("H500(1,V500(2,3))"));
    EXPECT(t.setParentRatio(3, 0.25));
    EXPECT(t.setParentRatio(1, 0.7)); // Either child names the same split.
    EXPECT_EQ(shape(), std::string("H700(1,V250(2,3))"));
    EXPECT(!t.setParentRatio(1, 0.0));  // Bad ratio.
    EXPECT(!t.setParentRatio(1, 1.0));
    EXPECT(!t.setParentRatio(99, 0.5)); // Unknown pane.
    Tree single;
    single.setRoot(5, make(5));
    EXPECT(!single.setParentRatio(5, 0.5)); // The root has no split above it.
}

TEST_SUIT_END
