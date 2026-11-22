#pragma once

// Between a live Workspace and a SessionSnapshot (docs/TABS_PLAN.md section 9).
//
// capture: the layout, the owner's tab titles, and for every pane what the frontend says
//   about it (folder, command that runs, optional screen text).
// restore: builds the same tabs and splits again through the Workspace's own commands, the way
//   the browser restores its layout. Panes come from the workspace factory, so the frontend
//   learns which saved pane the next one is for through `willCreate` (it sets the folder of the
//   shell it is about to start). Nothing is started from the snapshot except shells.
//
// A restored pane that had a program running gets an *offer*: `offerText()` says what was
// running and `rerunInput()` is the text to type into the shell, without a newline, so the owner
// sees the command on the prompt line and presses Enter, or edits it, or ignores it.
#include "closeprompt.h"
#include "paneprobe.h"
#include "sessionsnapshot.h"
#include "terminalmodel.h"
#include "workspace.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frontend {

// The text on the screen now (live rows only): trailing blanks and trailing empty lines cut,
// lines joined by '\n', cleaned and bounded like a snapshot's text.
inline std::string visibleText(const TerminalModel &model) {
    std::vector<std::string> lines;
    for (std::size_t y = 0; y < model.rows(); ++y) {
        std::size_t last = 0;
        for (std::size_t x = 0; x < model.columns(); ++x) {
            const char32_t cp = model.cell(x, y).codePoint;
            if (cp != U' ' && cp != 0) last = x + 1;
        }
        std::string line;
        for (std::size_t x = 0; x < last; ++x) {
            const char32_t cp = model.cell(x, y).codePoint;
            snapshot_detail::appendUtf8(line, cp == 0 ? U' ' : cp);
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) text += (i ? "\n" : "") + lines[i];
    return SessionSnapshot::cleanText(text);
}

// "was running: vim notes.txt - Enter to run it again" (empty if nothing was running).
inline std::string offerText(const SavedPane &pane) {
    const std::string line = commandLine(pane.command);
    if (line.empty()) return {};
    return "was running: " + CloseRequest::shown(line) + " - Enter to run it again";
}
// What to type into the shell for the offer (no newline); empty if there is no offer.
inline std::string rerunInput(const SavedPane &pane) {
    return commandLine(pane.command);
}

// `describe(paneId, pane)` returns the SavedPane (cwd, command, text) of a live pane; it is
// where PaneState (probePane) and visibleText() come together. nullopt when the layout
// cannot be saved (an empty workspace).
template <class Pane, class Describe>
std::optional<SessionSnapshot> captureSession(const Workspace<Pane> &workspace,
                                              const std::string &name,
                                              std::int64_t now,
                                              Describe &&describe) {
    if (workspace.tabCount() == 0) return std::nullopt;
    using NodePtr = std::unique_ptr<SavedNode>;
    SessionSnapshot s;
    s.name = name;
    s.saved = now;
    s.active = workspace.activeIndex();
    auto shapes = workspace.template foldTabs<NodePtr>(
        [&describe](PaneId id, const Pane &pane) { return SavedNode::leafOf(describe(id, pane)); },
        [](SplitDirection direction, double ratio, NodePtr a, NodePtr b) {
            const int permille = std::clamp(int(ratio * 1000.0 + 0.5), 1, 999);
            return SavedNode::split(direction, permille, std::move(a), std::move(b));
        });
    if (shapes.size() != workspace.tabCount()) return std::nullopt;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        SavedTab tab;
        tab.title = SessionSnapshot::cleanTitle(workspace.userTitle(i));
        const auto ids = workspace.tab(i)->tree.ids();
        const auto at = std::find(ids.begin(), ids.end(), workspace.tab(i)->tree.focus());
        tab.focus = at == ids.end() ? 0 : std::size_t(at - ids.begin());
        tab.root = std::move(shapes[i]);
        s.tabs.push_back(std::move(tab));
    }
    return s.valid() ? std::optional<SessionSnapshot>(std::move(s)) : std::nullopt;
}

struct RestoredPane {
    PaneId id = 0;
    const SavedPane *saved = nullptr; // Into the snapshot: it must outlive its use.
};

struct RestoreReport {
    std::vector<RestoredPane> panes; // Every pane that was made, in creation order.
    std::size_t tabs = 0;
    std::size_t refused = 0; // Tabs and splits the workspace would not make (limits, too small).
};

namespace restore_detail {
inline const SavedPane &firstLeaf(const SavedNode &n) {
    return n.leaf ? n.pane : firstLeaf(*n.first);
}
// Panes of a subtree in tree order are ids[from...]; the leaf at `index` is found by walking.
template <class Pane, class WillCreate>
void build(Workspace<Pane> &w, const SavedNode &node, PaneId pane, RestoreReport &report, WillCreate &willCreate) {
    if (node.leaf) return;
    w.focusPane(pane);
    willCreate(firstLeaf(*node.second));
    const Outcome o = w.apply(node.direction == SplitDirection::SideBySide ? Command::SplitSideBySide
                                                                           : Command::SplitStacked);
    PaneId created = 0;
    if (o == Outcome::Done) {
        created = w.focusedPaneId();
        report.panes.push_back({created, &firstLeaf(*node.second)});
        w.setParentRatio(created, node.permille / 1000.0);
    }
    else {
        ++report.refused;
    }
    build(w, *node.first, pane, report, willCreate);
    if (created != 0) build(w, *node.second, created, report, willCreate);
}
} // namespace restore_detail

// Makes the snapshot's tabs after the existing ones (an empty workspace gets exactly them) and
// selects the saved active tab when all came back. `willCreate(const SavedPane &)` is called
// right before each pane is made.
template <class Pane, class WillCreate>
RestoreReport restoreSession(Workspace<Pane> &workspace, const SessionSnapshot &snapshot, WillCreate &&willCreate) {
    RestoreReport report;
    const std::size_t firstTab = workspace.tabCount();
    for (const auto &tab : snapshot.tabs) {
        willCreate(restore_detail::firstLeaf(*tab.root));
        if (workspace.apply(Command::NewTab) != Outcome::Done) {
            ++report.refused;
            continue;
        }
        ++report.tabs;
        const PaneId root = workspace.focusedPaneId();
        report.panes.push_back({root, &restore_detail::firstLeaf(*tab.root)});
        const std::size_t index = workspace.activeIndex();
        workspace.setUserTitle(index, tab.title);
        restore_detail::build(workspace, *tab.root, root, report, willCreate);
        // The saved focus: the pane at that position in tree order.
        const auto ids = workspace.tab(index)->tree.ids();
        if (tab.focus < ids.size()) workspace.focusPane(ids[tab.focus]);
    }
    if (report.tabs == snapshot.tabs.size()) workspace.select(firstTab + snapshot.active);
    return report;
}

// When a window's session is saved (the owner decided: never automatic restore, saves only for a
// session the owner made persistent). One per window.
class SessionPolicy {
public:
    enum class Event { TabNamed, Shutdown };

    explicit SessionPolicy(std::string name = "default") : name_(std::move(name)) {}
    const std::string &name() const { return name_; }
    // Naming a tab marks this window's session as one worth keeping.
    bool persistent() const { return persistent_; }
    void adopt(std::string name) {
        name_ = std::move(name);
        persistent_ = true;
    }
    // True when the session should be written now. Shutdown (quit, SIGTERM, SIGHUP) writes
    // only a session the owner made persistent, and then with the screen text checkpoint.
    bool shouldSave(Event event) {
        if (event == Event::TabNamed) {
            persistent_ = true;
            return true;
        }
        return persistent_;
    }
    bool withScreenText(Event event) const { return event == Event::Shutdown && persistent_; }

    // The name "Save session as" offers: the name this window's session already has once
    // it is saved; otherwise the name of the tab (made into a file name: other characters
    // become '-'), and "default" when the tab has none.
    std::string suggestion(const std::string &tabTitle) const {
        if (persistent_) return name_;
        std::string s;
        for (const char c : tabTitle) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '_' || c == '-';
            const char out = ok ? c : '-';
            if (out == '-' && (s.empty() || s.back() == '-')) continue; // no runs, no leading '-'
            s += out;
        }
        while (!s.empty() && (s.back() == '-' || s.back() == '.')) s.pop_back();
        while (!s.empty() && s.front() == '.') s.erase(s.begin());
        if (s.size() > SessionSnapshot::kMaxName) s.resize(SessionSnapshot::kMaxName);
        return SessionSnapshot::validName(s) ? s : name_;
    }

private:
    std::string name_;
    bool persistent_ = false;
};

} // namespace frontend
