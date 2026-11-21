#pragma once

// The workspace of one window (docs/TABS_PLAN.md sections 2 and 3): tabs, each with a
// pane tree, plus the parked list. Pure logic; the pane type is a template parameter
// (a TerminalView with its link in the app, a counter in tests).
//
// Ownership: a pane is owned by exactly one place at a time: a leaf of a tab's tree or
// the parked list. Moving a pane between them is a std::move, closing destroys it.
// Everything else (focus, activity, the mouse) refers to panes by PaneId.
#include "panetree.h"
#include "tabtypes.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace frontend {

enum class Outcome {
    Done,    // something changed
    Ignored, // refused or nothing to do (too small, too many panes, no such tab...)
    Empty,   // the last tab is gone; the owner decides (quit, or a new tab)
};

template <class Pane>
class Workspace {
public:
    using Tree = PaneTree<Pane>;
    // Makes a fresh pane (starts its process); null if it cannot.
    using Factory = std::function<std::unique_ptr<Pane>()>;

    struct Tab {
        TabId id = 0;
        Tree tree;
        std::set<PaneId> activity; // hidden panes that had output or a bell
    };
    struct Parked {
        PaneId id = 0;
        std::unique_ptr<Pane> pane;
        bool activity = false;
    };

    explicit Workspace(Factory factory) : factory_(std::move(factory)) {}

    // The area below the tab strip and the cell metrics; set it on every resize.
    void setLayoutSpec(const LayoutSpec &spec) { spec_ = spec; }
    const LayoutSpec &layoutSpec() const { return spec_; }

    // ---- reading ----
    std::size_t tabCount() const { return tabs_.size(); }
    std::size_t activeIndex() const { return active_; }
    const Tab *tab(std::size_t index) const { return index < tabs_.size() ? &tabs_[index] : nullptr; }
    const Tab *activeTab() const { return tab(active_); }
    std::size_t parkedCount() const { return parked_.size(); }
    std::vector<PaneId> parkedIds() const {
        std::vector<PaneId> out;
        for (const auto &p : parked_) out.push_back(p.id);
        return out;
    }

    Layout layoutActive() const {
        const Tab *t = activeTab();
        return t ? t->tree.layout(spec_) : Layout{};
    }
    PaneId focusedPaneId() const {
        const Tab *t = activeTab();
        return t ? t->tree.focus() : 0;
    }
    Pane *focusedPane() const {
        const Tab *t = activeTab();
        return t ? t->tree.focusedPane() : nullptr;
    }
    // A pane anywhere: in any tab, visible or hidden, or parked.
    Pane *pane(PaneId id) const {
        for (const auto &t : tabs_) {
            if (Pane *p = t.tree.pane(id)) return p;
        }
        for (const auto &p : parked_) {
            if (p.id == id) return p.pane.get();
        }
        return nullptr;
    }
    // Index of the tab holding a pane, or -1 (parked or unknown).
    int tabOfPane(PaneId id) const {
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].tree.contains(id)) return static_cast<int>(i);
        }
        return -1;
    }
    int indexOfTab(TabId id) const {
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }
    std::size_t paneCount() const {
        std::size_t n = parked_.size();
        for (const auto &t : tabs_) n += t.tree.size();
        return n;
    }
    // Visit every pane once; `visible` is true for the panes of the active tab.
    template <class Visitor>
    void forEachPane(Visitor &&visit) const {
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            for (PaneId id : tabs_[i].tree.ids()) visit(id, *tabs_[i].tree.pane(id), i == active_);
        }
        for (const auto &p : parked_) visit(p.id, *p.pane, false);
    }

    // The shape of every tab, in tab order (see PaneTree::fold).
    template <class T, class OnLeaf, class OnSplit>
    std::vector<T> foldTabs(OnLeaf &&onLeaf, OnSplit &&onSplit) const {
        std::vector<T> out;
        for (const auto &t : tabs_) {
            if (!t.tree.empty()) out.push_back(t.tree.template fold<T>(onLeaf, onSplit));
        }
        return out;
    }
    // The ratio of the split above `id`, in whichever tab it is (restoring a shape).
    bool setParentRatio(PaneId id, double ratio) {
        for (auto &t : tabs_) {
            if (t.tree.contains(id)) return t.tree.setParentRatio(id, ratio);
        }
        return false;
    }

    // One entry per tab for the strip. A tab is titled by its focused pane.
    template <class TitleOf>
    std::vector<TabInfo> tabInfos(TitleOf &&titleOf) const {
        std::vector<TabInfo> out;
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            TabInfo info;
            info.id = tabs_[i].id;
            if (const Pane *p = tabs_[i].tree.focusedPane()) info.title = titleOf(*p);
            info.active = i == active_;
            info.activity = !tabs_[i].activity.empty();
            out.push_back(std::move(info));
        }
        return out;
    }

    // ---- activity ----
    // A pane produced output or rang the bell. Marks it only if it is hidden (in
    // another tab, or parked). True if a mark was set.
    bool noteActivity(PaneId id) {
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            if (!tabs_[i].tree.contains(id)) continue;
            if (i == active_) return false;
            return tabs_[i].activity.insert(id).second;
        }
        for (auto &p : parked_) {
            if (p.id == id) {
                const bool changed = !p.activity;
                p.activity = true;
                return changed;
            }
        }
        return false;
    }
    bool paneActivity(PaneId id) const {
        for (const auto &t : tabs_) {
            if (t.activity.count(id) != 0) return true;
        }
        for (const auto &p : parked_) {
            if (p.id == id) return p.activity;
        }
        return false;
    }

    // ---- commands ----
    // `argument` is the tab index of SelectTab.
    Outcome apply(Command command, int argument = 0) {
        switch (command) {
        case Command::NewTab:
            return newTab();
        case Command::CloseTab:
            return tabs_.empty() ? Outcome::Ignored : closeTabAt(active_);
        case Command::NextTab:
            return tabs_.size() < 2 ? Outcome::Ignored : select((active_ + 1) % tabs_.size());
        case Command::PreviousTab:
            return tabs_.size() < 2 ? Outcome::Ignored : select((active_ + tabs_.size() - 1) % tabs_.size());
        case Command::SelectTab:
            return argument < 0 ? Outcome::Ignored : select(static_cast<std::size_t>(argument));
        case Command::SplitSideBySide:
            return split(SplitDirection::SideBySide);
        case Command::SplitStacked:
            return split(SplitDirection::Stacked);
        case Command::ClosePane:
            return tabs_.empty() ? Outcome::Ignored : closePane(focusedPaneId());
        case Command::FocusLeft:
            return focusToward(FocusDirection::Left);
        case Command::FocusRight:
            return focusToward(FocusDirection::Right);
        case Command::FocusUp:
            return focusToward(FocusDirection::Up);
        case Command::FocusDown:
            return focusToward(FocusDirection::Down);
        case Command::EvenOut:
            if (tabs_.empty()) return Outcome::Ignored;
            tabs_[active_].tree.evenOut(spec_);
            return Outcome::Done;
        }
        return Outcome::Ignored;
    }

    // ---- direct operations (mouse, strip, drag and drop) ----
    // Select a tab; clears its activity marks. Selecting the active tab is ignored.
    Outcome select(std::size_t index) {
        if (index >= tabs_.size() || index == active_) return Outcome::Ignored;
        active_ = index;
        tabs_[active_].activity.clear();
        return Outcome::Done;
    }
    // Close one pane anywhere in the active tab; the last pane closes the tab.
    Outcome closePane(PaneId id) {
        if (tabs_.empty()) return Outcome::Ignored;
        Tab &t = tabs_[active_];
        if (!t.tree.contains(id)) return Outcome::Ignored;
        std::unique_ptr<Pane> gone = t.tree.close(id); // destroyed at the end of the scope
        if (t.tree.empty()) return removeTab(active_);
        return Outcome::Done;
    }
    // Close a pane wherever it is: any tab (the tab closes with its last pane) or the
    // parked list. For panes whose process ended in a hidden tab.
    Outcome closePaneAnywhere(PaneId id) {
        const int i = tabOfPane(id);
        if (i < 0) return closeParked(id);
        const std::size_t index = static_cast<std::size_t>(i);
        std::unique_ptr<Pane> gone = tabs_[index].tree.close(id); // destroyed at the end of the scope
        tabs_[index].activity.erase(id);
        if (tabs_[index].tree.empty()) return removeTab(index);
        return Outcome::Done;
    }
    Outcome closeTabAt(std::size_t index) {
        if (index >= tabs_.size()) return Outcome::Ignored;
        return removeTab(index);
    }
    // Focus a pane by id (a click); switches to its tab if needed.
    Outcome focusPane(PaneId id) {
        const int i = tabOfPane(id);
        if (i < 0) return Outcome::Ignored;
        const std::size_t index = static_cast<std::size_t>(i);
        Outcome result = Outcome::Ignored;
        if (index != active_) result = select(index);
        if (tabs_[index].tree.focus() != id) {
            tabs_[index].tree.setFocus(id);
            result = Outcome::Done;
        }
        return result;
    }
    // Move a tab to a new position (drag in the strip). The active tab stays active.
    Outcome moveTab(std::size_t from, std::size_t to) {
        if (from >= tabs_.size() || to >= tabs_.size() || from == to) return Outcome::Ignored;
        const TabId activeId = tabs_[active_].id;
        Tab moved = std::move(tabs_[from]);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        active_ = static_cast<std::size_t>(indexOfTab(activeId));
        return Outcome::Done;
    }
    // Divider drag on the active tab, see PaneTree::dragDivider.
    Outcome dragDivider(SplitId split, int pointer, bool snap) {
        if (tabs_.empty()) return Outcome::Ignored;
        return tabs_[active_].tree.dragDivider(split, pointer, snap, spec_) ? Outcome::Done : Outcome::Ignored;
    }

    // Take a pane out of its tab and keep it running, not drawn. The tab closes if it
    // was its last pane.
    Outcome park(PaneId id) {
        const int i = tabOfPane(id);
        if (i < 0) return Outcome::Ignored;
        const std::size_t index = static_cast<std::size_t>(i);
        Tab &t = tabs_[index];
        std::unique_ptr<Pane> p = t.tree.close(id);
        const bool hadActivity = t.activity.erase(id) != 0;
        parked_.push_back({id, std::move(p), hadActivity});
        if (t.tree.empty()) return removeTab(index);
        return Outcome::Done;
    }
    // Bring a parked pane back in a new tab after the active one.
    Outcome unparkToNewTab(PaneId id) {
        const auto it = findParked(id);
        if (it == parked_.end()) return Outcome::Ignored;
        std::unique_ptr<Pane> p = std::move(it->pane);
        parked_.erase(it);
        insertTab(id, std::move(p));
        return Outcome::Done;
    }
    // Bring a parked pane back next to the focused pane. If the split is refused
    // (too small, too many panes) the pane stays parked.
    Outcome unparkSplit(PaneId id, SplitDirection direction) {
        const auto it = findParked(id);
        if (it == parked_.end() || tabs_.empty()) return Outcome::Ignored;
        Tab &t = tabs_[active_];
        const SplitResult r = t.tree.split(t.tree.focus(), direction, id, it->pane, spec_);
        if (r != SplitResult::Ok) return Outcome::Ignored;
        parked_.erase(it);
        return Outcome::Done;
    }
    // Close a parked pane (destroys it).
    Outcome closeParked(PaneId id) {
        const auto it = findParked(id);
        if (it == parked_.end()) return Outcome::Ignored;
        parked_.erase(it);
        return Outcome::Done;
    }

private:
    using ParkedList = std::vector<Parked>;

    typename ParkedList::iterator findParked(PaneId id) {
        return std::find_if(parked_.begin(), parked_.end(), [id](const Parked &p) { return p.id == id; });
    }

    // New tab after the active one, made active.
    void insertTab(PaneId paneId, std::unique_ptr<Pane> p) {
        Tab t;
        t.id = ++lastTab_;
        t.tree.setRoot(paneId, std::move(p));
        const std::size_t at = tabs_.empty() ? 0 : active_ + 1;
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(t));
        active_ = at;
    }

    Outcome newTab() {
        std::unique_ptr<Pane> p = factory_ ? factory_() : nullptr;
        if (!p) return Outcome::Ignored;
        insertTab(++lastPane_, std::move(p));
        return Outcome::Done;
    }

    Outcome split(SplitDirection direction) {
        if (tabs_.empty()) return Outcome::Ignored;
        Tab &t = tabs_[active_];
        // Ask before making a pane: making one starts a process.
        if (t.tree.canSplit(t.tree.focus(), direction, spec_) != SplitResult::Ok) return Outcome::Ignored;
        std::unique_ptr<Pane> p = factory_ ? factory_() : nullptr;
        if (!p) return Outcome::Ignored;
        return t.tree.split(t.tree.focus(), direction, ++lastPane_, p, spec_) == SplitResult::Ok
                   ? Outcome::Done
                   : Outcome::Ignored;
    }

    Outcome focusToward(FocusDirection direction) {
        if (tabs_.empty()) return Outcome::Ignored;
        return tabs_[active_].tree.moveFocus(direction, spec_) ? Outcome::Done : Outcome::Ignored;
    }

    // Remove a tab (its panes are destroyed). The tab that follows becomes active, or
    // the previous one if it was the last.
    Outcome removeTab(std::size_t index) {
        const bool wasActive = index == active_;
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (tabs_.empty()) {
            active_ = 0;
            return Outcome::Empty;
        }
        if (index < active_) {
            --active_;
        }
        else if (wasActive && active_ >= tabs_.size()) {
            active_ = tabs_.size() - 1;
        }
        if (wasActive) tabs_[active_].activity.clear();
        return Outcome::Done;
    }

    Factory factory_;
    LayoutSpec spec_;
    std::vector<Tab> tabs_;
    ParkedList parked_;
    std::size_t active_ = 0;
    PaneId lastPane_ = 0;
    TabId lastTab_ = 0;
};

} // namespace frontend
