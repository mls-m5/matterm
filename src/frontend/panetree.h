#pragma once

// The pane tree of one tab (docs/TABS_PLAN.md section 3): a binary tree whose leaves
// own the panes and whose inner nodes are splits with a ratio. Pure logic, no SDL.
//
// Layout is in pixels. A split gives its first child round(ratio * available) pixels
// (available = extent minus the divider gutter), clamped so that both subtrees keep
// their minimum size; the second child gets the rest. The boxes of the panes and the
// gutters therefore tile the area exactly: no pixel is lost, gained or covered twice.
// A pane's program sees only the whole cells that fit; the rest is `leftover`.
#include "tabtypes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace frontend {

constexpr std::size_t kMaxPanesPerTab = 8;
constexpr int kMinPaneColumns = 20;
constexpr int kMinPaneRows = 5;

using SplitId = std::uint32_t; // 0 is "none"

// Everything layout needs to know besides the tree.
struct LayoutSpec {
    PixelRect area;
    int cellWidth = 1;
    int cellHeight = 1;
    int dividerThickness = 0;
    int minColumns = kMinPaneColumns;
    int minRows = kMinPaneRows;
};

struct Layout {
    std::vector<PaneBox> panes;           // tree order: left or upper first
    std::vector<DividerBox> dividers;     // outer splits before inner ones
    std::vector<SplitId> dividerSplits;   // dividerSplits[i] is the split of dividers[i]

    const PaneBox *box(PaneId id) const {
        for (const auto &p : panes) {
            if (p.pane == id) return &p;
        }
        return nullptr;
    }
    PaneId paneAt(int x, int y) const {
        for (const auto &p : panes) {
            if (contains(p.rect, x, y)) return p.pane;
        }
        return 0;
    }
    // Index into dividers of the gutter under a point, or -1.
    int dividerAt(int x, int y) const {
        for (std::size_t i = 0; i < dividers.size(); ++i) {
            if (contains(dividers[i].rect, x, y)) return static_cast<int>(i);
        }
        return -1;
    }
    static bool contains(const PixelRect &r, int x, int y) {
        return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
    }
};

enum class FocusDirection { Left, Right, Up, Down };

enum class SplitResult { Ok, TooManyPanes, TooSmall, BadArgument };

// Pane is anything movable behind a unique_ptr (a TerminalView and its link in the
// app, a counter in tests). The tree owns a pane from the moment split() or setRoot()
// succeeds until close() hands it back or the tree is destroyed.
template <class Pane>
class PaneTree {
public:
    PaneTree() = default;
    PaneTree(PaneTree &&) noexcept = default;
    PaneTree &operator=(PaneTree &&) noexcept = default;
    PaneTree(const PaneTree &) = delete;
    PaneTree &operator=(const PaneTree &) = delete;

    bool empty() const { return !root_; }
    std::size_t size() const { return count_; }
    PaneId focus() const { return focus_; }
    bool contains(PaneId id) const { return find(root_.get(), id) != nullptr; }

    Pane *pane(PaneId id) const {
        const Node *n = find(root_.get(), id);
        return n ? n->pane.get() : nullptr;
    }
    Pane *focusedPane() const { return pane(focus_); }

    // Ids in tree order.
    std::vector<PaneId> ids() const {
        std::vector<PaneId> out;
        collect(root_.get(), out);
        return out;
    }

    // Put the first pane into an empty tree. It gets the focus.
    bool setRoot(PaneId id, std::unique_ptr<Pane> p) {
        if (root_ || id == 0 || !p) return false;
        root_ = makeLeaf(id, std::move(p));
        count_ = 1;
        focus_ = id;
        return true;
    }

    // Can `target` be split in `direction` right now? Tests the count and the minimum
    // size (two panes of at least the minimum and a gutter must fit in its box).
    SplitResult canSplit(PaneId target, SplitDirection direction, const LayoutSpec &spec) const {
        if (!contains(target)) return SplitResult::BadArgument;
        if (count_ >= kMaxPanesPerTab) return SplitResult::TooManyPanes;
        const Layout l = layout(spec);
        const PaneBox *b = l.box(target);
        if (b == nullptr) return SplitResult::BadArgument;
        const bool side = direction == SplitDirection::SideBySide;
        const int need = side ? 2 * spec.minColumns * spec.cellWidth + thickness(spec)
                              : 2 * spec.minRows * spec.cellHeight + thickness(spec);
        return (side ? b->rect.w : b->rect.h) >= need ? SplitResult::Ok : SplitResult::TooSmall;
    }

    // Split `target`; the new pane goes right of or below it, takes half the space and
    // gets the focus. On any failure `p` is untouched, so the caller keeps ownership.
    SplitResult split(PaneId target, SplitDirection direction, PaneId newId,
                      std::unique_ptr<Pane> &p, const LayoutSpec &spec) {
        if (newId == 0 || !p || contains(newId)) return SplitResult::BadArgument;
        const SplitResult ok = canSplit(target, direction, spec);
        if (ok != SplitResult::Ok) return ok;
        std::unique_ptr<Node> *slot = findSlot(root_, target);
        if (slot == nullptr) return SplitResult::BadArgument;
        auto node = std::make_unique<Node>();
        node->direction = direction;
        node->splitId = ++lastSplit_;
        node->first = std::move(*slot);
        node->second = makeLeaf(newId, std::move(p));
        *slot = std::move(node);
        ++count_;
        focus_ = newId;
        return SplitResult::Ok;
    }

    // Remove a pane; its sibling takes the space. Returns the pane (null if unknown).
    // If it had the focus, the focus goes to the pane that took its place.
    std::unique_ptr<Pane> close(PaneId id) {
        std::unique_ptr<Pane> out;
        if (!root_) return out;
        if (root_->leaf()) {
            if (root_->id != id) return out;
            out = std::move(root_->pane);
            root_.reset();
            count_ = 0;
            focus_ = 0;
            return out;
        }
        PaneId heir = 0;
        if (!remove(root_, id, out, heir)) return out;
        --count_;
        if (focus_ == id) focus_ = heir;
        return out;
    }

    bool setFocus(PaneId id) {
        if (!contains(id)) return false;
        focus_ = id;
        return true;
    }

    // Move the focus to the neighbour in a direction, by geometry. Among the panes that
    // lie entirely on that side and share some of the edge, the nearest wins, then the
    // one sharing most of it, then the first in tree order. False if nothing is there.
    bool moveFocus(FocusDirection direction, const LayoutSpec &spec) {
        const PaneId to = neighbour(focus_, direction, spec);
        if (to == 0) return false;
        focus_ = to;
        return true;
    }

    PaneId neighbour(PaneId from, FocusDirection direction, const LayoutSpec &spec) const {
        const Layout l = layout(spec);
        const PaneBox *me = l.box(from);
        if (me == nullptr) return 0;
        const PixelRect &r = me->rect;
        PaneId best = 0;
        long long bestGap = 0, bestOverlap = 0;
        for (const auto &c : l.panes) {
            if (c.pane == from) continue;
            long long gap = 0, overlap = 0;
            switch (direction) {
            case FocusDirection::Left:
                gap = r.x - (c.rect.x + c.rect.w);
                overlap = span(r.y, r.h, c.rect.y, c.rect.h);
                break;
            case FocusDirection::Right:
                gap = c.rect.x - (r.x + r.w);
                overlap = span(r.y, r.h, c.rect.y, c.rect.h);
                break;
            case FocusDirection::Up:
                gap = r.y - (c.rect.y + c.rect.h);
                overlap = span(r.x, r.w, c.rect.x, c.rect.w);
                break;
            case FocusDirection::Down:
                gap = c.rect.y - (r.y + r.h);
                overlap = span(r.x, r.w, c.rect.x, c.rect.w);
                break;
            }
            if (gap < 0 || overlap <= 0) continue;
            if (best == 0 || gap < bestGap || (gap == bestGap && overlap > bestOverlap)) {
                best = c.pane;
                bestGap = gap;
                bestOverlap = overlap;
            }
        }
        return best;
    }

    // Boxes and gutters for an area. Never throws, also for areas below the minimum:
    // then the space is shared in proportion to the minimums.
    Layout layout(const LayoutSpec &spec) const {
        Layout out;
        if (root_) place(*root_, clamped(spec.area), spec, out);
        return out;
    }
    Layout layout(PixelRect area, int cellWidth, int cellHeight, int dividerThickness) const {
        LayoutSpec s;
        s.area = area;
        s.cellWidth = cellWidth;
        s.cellHeight = cellHeight;
        s.dividerThickness = dividerThickness;
        return layout(s);
    }

    // Smallest area (w, h) in which every pane gets its minimum.
    std::pair<int, int> minimumSize(const LayoutSpec &spec) const {
        if (!root_) return {0, 0};
        return {minExtent(*root_, true, spec), minExtent(*root_, false, spec)};
    }

    // Set a split's ratio directly (0 < ratio < 1, clamped by the minimums at layout).
    bool setRatio(SplitId split, double ratio) {
        Node *n = findSplit(root_.get(), split);
        if (n == nullptr || !(ratio > 0.0) || !(ratio < 1.0)) return false;
        n->ratio = ratio;
        return true;
    }
    std::optional<double> ratio(SplitId split) const {
        const Node *n = findSplit(root_.get(), split);
        if (n == nullptr) return std::nullopt;
        return n->ratio;
    }

    // Divider drag: `pointer` is where the pointer is along the split's axis (x for a
    // vertical divider, y for a horizontal one), in the same pixels as the area; the
    // divider's centre follows it. Clamped by the minimums; with `snap` the first
    // child gets a whole number of cells. True if the ratio changed.
    bool dragDivider(SplitId split, int pointer, bool snap, const LayoutSpec &spec) {
        if (!root_) return false;
        Node *n = nullptr;
        PixelRect rect;
        if (!findSplitRect(*root_, clamped(spec.area), spec, split, n, rect)) return false;
        const bool side = n->direction == SplitDirection::SideBySide;
        const Division d = divide(*n, rect, spec);
        const int start = side ? rect.x : rect.y;
        const int want = pointer - start - d.gutter / 2;
        const int first = fit(*n, d, want, snap, spec);
        if (d.available <= 0) return false;
        const double before = n->ratio;
        n->ratio = static_cast<double>(first) / static_cast<double>(d.available);
        return n->ratio != before;
    }

    // Equal shares by the number of panes along the split's axis, every split snapped
    // to whole cells (as far as the minimums allow).
    void evenOut(const LayoutSpec &spec) {
        if (root_) even(*root_, clamped(spec.area), spec);
    }

private:
    struct Node {
        SplitDirection direction = SplitDirection::SideBySide;
        double ratio = 0.5;
        SplitId splitId = 0;
        std::unique_ptr<Node> first, second; // both null: a leaf
        PaneId id = 0;
        std::unique_ptr<Pane> pane;
        bool leaf() const { return !first; }
    };

    struct Division {
        PixelRect first, second, gutterRect;
        int available = 0; // extent minus the gutter
        int firstExtent = 0;
        int gutter = 0;
        int lo = 0, hi = 0; // range allowed for firstExtent
    };

    static PixelRect clamped(PixelRect r) {
        r.w = std::max(r.w, 0);
        r.h = std::max(r.h, 0);
        return r;
    }
    static int thickness(const LayoutSpec &s) { return std::max(s.dividerThickness, 0); }
    static long long span(int a, int alen, int b, int blen) {
        return static_cast<long long>(std::min(a + alen, b + blen)) - std::max(a, b);
    }

    static std::unique_ptr<Node> makeLeaf(PaneId id, std::unique_ptr<Pane> p) {
        auto n = std::make_unique<Node>();
        n->id = id;
        n->pane = std::move(p);
        return n;
    }

    static const Node *find(const Node *n, PaneId id) {
        if (n == nullptr) return nullptr;
        if (n->leaf()) return n->id == id ? n : nullptr;
        if (const Node *f = find(n->first.get(), id)) return f;
        return find(n->second.get(), id);
    }
    static Node *findSplit(Node *n, SplitId id) {
        if (n == nullptr || n->leaf()) return nullptr;
        if (n->splitId == id) return n;
        if (Node *f = findSplit(n->first.get(), id)) return f;
        return findSplit(n->second.get(), id);
    }
    static const Node *findSplit(const Node *n, SplitId id) {
        return findSplit(const_cast<Node *>(n), id);
    }
    static std::unique_ptr<Node> *findSlot(std::unique_ptr<Node> &slot, PaneId id) {
        if (!slot) return nullptr;
        if (slot->leaf()) return slot->id == id ? &slot : nullptr;
        if (auto *f = findSlot(slot->first, id)) return f;
        return findSlot(slot->second, id);
    }
    static void collect(const Node *n, std::vector<PaneId> &out) {
        if (n == nullptr) return;
        if (n->leaf()) {
            out.push_back(n->id);
            return;
        }
        collect(n->first.get(), out);
        collect(n->second.get(), out);
    }
    static PaneId firstLeaf(const Node &n) { return n.leaf() ? n.id : firstLeaf(*n.first); }
    static PaneId lastLeaf(const Node &n) { return n.leaf() ? n.id : lastLeaf(*n.second); }

    // Remove leaf `id` below `slot` (which is a split). The parent split is replaced by
    // the sibling. `heir` is the leaf of the sibling nearest to the removed pane.
    static bool remove(std::unique_ptr<Node> &slot, PaneId id, std::unique_ptr<Pane> &out, PaneId &heir) {
        if (!slot || slot->leaf()) return false;
        std::unique_ptr<Node> &a = slot->first;
        std::unique_ptr<Node> &b = slot->second;
        if (a->leaf() && a->id == id) {
            out = std::move(a->pane);
            heir = firstLeaf(*b);
            std::unique_ptr<Node> keep = std::move(b);
            slot = std::move(keep);
            return true;
        }
        if (b->leaf() && b->id == id) {
            out = std::move(b->pane);
            heir = lastLeaf(*a);
            std::unique_ptr<Node> keep = std::move(a);
            slot = std::move(keep);
            return true;
        }
        return remove(a, id, out, heir) || remove(b, id, out, heir);
    }

    // Minimum extent of a subtree along x (`horizontal`) or y.
    static int minExtent(const Node &n, bool horizontal, const LayoutSpec &s) {
        if (n.leaf()) {
            return horizontal ? std::max(s.minColumns, 1) * std::max(s.cellWidth, 1)
                              : std::max(s.minRows, 1) * std::max(s.cellHeight, 1);
        }
        const int a = minExtent(*n.first, horizontal, s);
        const int b = minExtent(*n.second, horizontal, s);
        const bool alongAxis = (n.direction == SplitDirection::SideBySide) == horizontal;
        return alongAxis ? a + thickness(s) + b : std::max(a, b);
    }
    // Panes along an axis, for even().
    static int weight(const Node &n, bool horizontal) {
        if (n.leaf()) return 1;
        const int a = weight(*n.first, horizontal);
        const int b = weight(*n.second, horizontal);
        const bool alongAxis = (n.direction == SplitDirection::SideBySide) == horizontal;
        return alongAxis ? a + b : std::max(a, b);
    }

    static Division divide(const Node &n, PixelRect r, const LayoutSpec &s) {
        const bool side = n.direction == SplitDirection::SideBySide;
        Division d;
        const int extent = side ? r.w : r.h;
        d.gutter = std::min(thickness(s), extent);
        d.available = extent - d.gutter;
        const int minA = minExtent(*n.first, side, s);
        const int minB = minExtent(*n.second, side, s);
        d.lo = minA;
        d.hi = d.available - minB;
        if (d.lo > d.hi) {
            // Not enough room for both minimums: share in proportion to them.
            const long long total = static_cast<long long>(minA) + minB;
            d.firstExtent = total > 0 ? static_cast<int>(static_cast<long long>(d.available) * minA / total)
                                      : d.available / 2;
            d.lo = d.hi = d.firstExtent;
        }
        else {
            const long long want = std::llround(n.ratio * static_cast<double>(d.available));
            d.firstExtent = static_cast<int>(std::clamp<long long>(want, d.lo, d.hi));
        }
        arrange(d, side, r);
        return d;
    }
    static void arrange(Division &d, bool side, PixelRect r) {
        const int rest = d.available - d.firstExtent;
        if (side) {
            d.first = {r.x, r.y, d.firstExtent, r.h};
            d.gutterRect = {r.x + d.firstExtent, r.y, d.gutter, r.h};
            d.second = {r.x + d.firstExtent + d.gutter, r.y, rest, r.h};
        }
        else {
            d.first = {r.x, r.y, r.w, d.firstExtent};
            d.gutterRect = {r.x, r.y + d.firstExtent, r.w, d.gutter};
            d.second = {r.x, r.y + d.firstExtent + d.gutter, r.w, rest};
        }
    }

    // Choose the first child's extent for `want`, clamped and optionally snapped.
    static int fit(const Node &n, const Division &d, int want, bool snap, const LayoutSpec &s) {
        int first = std::clamp(want, d.lo, std::max(d.lo, d.hi));
        if (snap) {
            const bool side = n.direction == SplitDirection::SideBySide;
            const int unit = std::max(side ? s.cellWidth : s.cellHeight, 1);
            const int kmin = (d.lo + unit - 1) / unit;
            const int kmax = d.hi / unit;
            if (kmin <= kmax) {
                const int k = std::clamp((first + unit / 2) / unit, kmin, kmax);
                first = k * unit;
            }
        }
        return first;
    }

    static void place(const Node &n, PixelRect r, const LayoutSpec &s, Layout &out) {
        if (n.leaf()) {
            PaneBox b;
            b.pane = n.id;
            b.rect = r;
            b.columns = r.w / std::max(s.cellWidth, 1);
            b.rows = r.h / std::max(s.cellHeight, 1);
            const int usedW = b.columns * std::max(s.cellWidth, 1);
            const int usedH = b.rows * std::max(s.cellHeight, 1);
            // [0] right of the last column over the full height, [1] below the last
            // row under the whole columns only, so the two never overlap.
            b.leftover[0] = {r.x + usedW, r.y, r.w - usedW, r.h};
            b.leftover[1] = {r.x, r.y + usedH, usedW, r.h - usedH};
            out.panes.push_back(b);
            return;
        }
        const Division d = divide(n, r, s);
        DividerBox g;
        g.rect = d.gutterRect;
        g.direction = n.direction;
        g.before = nearestLeaf(*n.first, n.direction, true);
        g.after = nearestLeaf(*n.second, n.direction, false);
        out.dividers.push_back(g);
        out.dividerSplits.push_back(n.splitId);
        place(*n.first, d.first, s, out);
        place(*n.second, d.second, s, out);
    }
    // The leaf of a subtree that touches the gutter: the last one of the first child
    // along the axis, the first one of the second child.
    static PaneId nearestLeaf(const Node &n, SplitDirection axis, bool towardEnd) {
        if (n.leaf()) return n.id;
        if (n.direction == axis) {
            return nearestLeaf(towardEnd ? *n.second : *n.first, axis, towardEnd);
        }
        // Crossing split: both children touch the gutter, take the first.
        return nearestLeaf(*n.first, axis, towardEnd);
    }

    static bool findSplitRect(Node &n, PixelRect r, const LayoutSpec &s, SplitId id, Node *&found, PixelRect &rect) {
        if (n.leaf()) return false;
        if (n.splitId == id) {
            found = &n;
            rect = r;
            return true;
        }
        const Division d = divide(n, r, s);
        return findSplitRect(*n.first, d.first, s, id, found, rect) ||
               findSplitRect(*n.second, d.second, s, id, found, rect);
    }

    static void even(Node &n, PixelRect r, const LayoutSpec &s) {
        if (n.leaf()) return;
        const bool side = n.direction == SplitDirection::SideBySide;
        const int a = weight(*n.first, side);
        const int b = weight(*n.second, side);
        n.ratio = static_cast<double>(a) / static_cast<double>(a + b);
        Division d = divide(n, r, s);
        if (d.available > 0) {
            const int first = fit(n, d, d.firstExtent, true, s);
            n.ratio = static_cast<double>(first) / static_cast<double>(d.available);
            d = divide(n, r, s);
        }
        even(*n.first, d.first, s);
        even(*n.second, d.second, s);
    }

    std::unique_ptr<Node> root_;
    std::size_t count_ = 0;
    PaneId focus_ = 0;
    SplitId lastSplit_ = 0;
};

} // namespace frontend
