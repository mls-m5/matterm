#pragma once

// The one list of everything the user can ask for, with names, descriptions and the
// keys that run it (docs/TABS_PLAN.md section 8). The key strings are not typed in a
// second place: they are produced from the prefix bindings and the desktop shortcut
// table, so the palette, the help view and the keys cannot disagree.
#include "fuzzymatch.h"
#include "prefixkeys.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frontend {

// "Ctrl+Shift+T", "Alt+Shift+Left", "PgDn". Letters print upper case.
inline std::string formatChord(const KeyChord &k) {
    std::string s;
    if (k.ctrl) s += "Ctrl+";
    if (k.alt) s += "Alt+";
    if (k.shift) s += "Shift+";
    switch (k.key) {
    case keys::Up: return s + "Up";
    case keys::Down: return s + "Down";
    case keys::Left: return s + "Left";
    case keys::Right: return s + "Right";
    case keys::PageUp: return s + "PgUp";
    case keys::PageDown: return s + "PgDn";
    case keys::Escape: return s + "Esc";
    case keys::Enter: return s + "Enter";
    case keys::Tab: return s + "Tab";
    case keys::Backspace: return s + "Backspace";
    default: break;
    }
    const int c = foldKey(k.key);
    if (c >= 'a' && c <= 'z') return s + char(c - 'a' + 'A');
    if (c > 32 && c < 127) return s + char(c);
    return s + "?";
}

struct CommandEntry {
    std::string id;          // stable, for settings and tests: "tab.new"
    std::string name;        // shown and searched: "New tab"
    std::string description; // one line
    // Set for layout commands. Entries without a call are actions of the application
    // itself (palette, help, rename ...), found by `id`.
    std::optional<CommandCall> call;
    std::vector<std::string> prefixKeys;  // "Ctrl+B c" (browser), empty if none
    std::vector<std::string> desktopKeys; // "Ctrl+Shift+T" (desktop app), empty if none
};

namespace registrydetail {
struct Info {
    Command command;
    int argument;
    const char *id, *name, *description;
};
inline const std::vector<Info> &layoutInfo() {
    static const std::vector<Info> table = {
        {Command::NewTab, 0, "tab.new", "New tab", "Open a new tab with a fresh terminal"},
        {Command::CloseTab, 0, "tab.close", "Close tab", "Close the tab and all its terminals"},
        {Command::NextTab, 0, "tab.next", "Next tab", "Show the next tab"},
        {Command::PreviousTab, 0, "tab.previous", "Previous tab", "Show the previous tab"},
        {Command::SplitSideBySide, 0, "pane.split-right", "Split side by side",
         "Split the terminal into a left and a right one"},
        {Command::SplitStacked, 0, "pane.split-down", "Split stacked",
         "Split the terminal into an upper and a lower one"},
        {Command::ClosePane, 0, "pane.close", "Close terminal", "Close the terminal that has focus"},
        {Command::FocusLeft, 0, "pane.focus-left", "Focus left", "Move focus to the terminal on the left"},
        {Command::FocusRight, 0, "pane.focus-right", "Focus right", "Move focus to the terminal on the right"},
        {Command::FocusUp, 0, "pane.focus-up", "Focus up", "Move focus to the terminal above"},
        {Command::FocusDown, 0, "pane.focus-down", "Focus down", "Move focus to the terminal below"},
        {Command::EvenOut, 0, "pane.even-out", "Even out splits",
         "Give the terminals of the tab equal shares, snapped to whole cells"},
        {Command::OpenPalette, 0, "app.palette", "Command palette",
         "Search all commands and run one"},
    };
    return table;
}
} // namespace registrydetail

// All entries: the layout commands, "Go to tab 1..9", then `extra` (the application's
// own actions, which have no call).
inline std::vector<CommandEntry> buildCommandRegistry(
    const PrefixKeys::Config &prefix = PrefixKeys::Config{},
    const std::vector<CommandEntry> &extra = {}) {
    std::vector<CommandEntry> out;
    auto keysFor = [&](Command c, int arg, CommandEntry &e) {
        for (const auto &b : prefix.bindings) {
            if (b.call.command == c && b.call.argument == arg) {
                e.prefixKeys.push_back(formatChord(prefix.prefix) + " " +
                                       formatChord({b.key, b.ctrl, false, false}));
            }
        }
        for (const auto &d : desktopShortcuts()) {
            if (d.action == DesktopAction::Layout && d.call.command == c && d.call.argument == arg) {
                e.desktopKeys.push_back(formatChord(d.chord));
            }
        }
    };
    for (const auto &i : registrydetail::layoutInfo()) {
        CommandEntry e{i.id, i.name, i.description, CommandCall{i.command, i.argument}, {}, {}};
        keysFor(i.command, i.argument, e);
        out.push_back(std::move(e));
    }
    for (int d = 1; d <= 9; ++d) {
        CommandEntry e{"tab.go-" + std::to_string(d), "Go to tab " + std::to_string(d),
                       "Show tab number " + std::to_string(d),
                       CommandCall{Command::SelectTab, d - 1}, {}, {}};
        keysFor(Command::SelectTab, d - 1, e);
        out.push_back(std::move(e));
    }
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

struct RankedEntry {
    const CommandEntry *entry = nullptr;
    FuzzyMatch match; // offsets are into entry->name; empty when matched on the id/description
};

// The entries for a query, best first; with an empty query all entries in list order.
// Searches the name first, then the id and the description at a lower score.
inline std::vector<RankedEntry> searchCommands(const std::vector<CommandEntry> &entries,
                                               std::string_view query) {
    std::vector<RankedEntry> found;
    for (const auto &e : entries) {
        RankedEntry r{&e, fuzzyMatch(query, e.name)};
        if (!r.match.matched) {
            auto other = fuzzyMatch(query, e.id);
            if (!other.matched) other = fuzzyMatch(query, e.description);
            if (!other.matched) continue;
            r.match = other;
            r.match.score -= 50;
            r.match.offsets.clear();
        }
        found.push_back(std::move(r));
    }
    std::stable_sort(found.begin(), found.end(), [](const RankedEntry &a, const RankedEntry &b) {
        return a.match.score > b.match.score;
    });
    return found;
}

} // namespace frontend
