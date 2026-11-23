#pragma once

// The palette actions that belong to the local daemon (docs/tasks/daemon.md):
// stop it, quit the window but leave the programs running, show what it holds.
// Only what the front end needs to offer them: the entries, the confirmation
// text, the status text. No sockets here; the application does the work when
// the action comes back.
#include "commandregistry.h"
#include "sessionsnapshot.h"
#include "ui/dialogs.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace frontend {

namespace daemonaction {
inline constexpr const char *stop = "daemon.stop"; // Opens the confirmation.
inline constexpr const char *stopConfirm =
    "daemon.stop.confirm"; // The user agreed.
inline constexpr const char *stopCancel = "daemon.stop.cancel";
inline constexpr const char *quitKeep =
    "daemon.quitkeep"; // Close the window only.
inline constexpr const char *status = "daemon.status";
// --daemon-host: start the ssh connection again now (the terminals there are
// attached again).
inline constexpr const char *reconnect = "daemon.reconnect";
// Sharing (the daemon's per-terminal mark): the pane in focus or every pane of
// the active tab.
inline constexpr const char *sharePane = "share.pane";
inline constexpr const char *sharePaneStop = "share.pane.stop";
inline constexpr const char *shareTab = "share.tab";
inline constexpr const char *shareTabStop = "share.tab.stop";
// Terminals that live in the daemon: attach one (a list, then "attach.go:<id>"), attach all
// that no window shows, detach the focused one (it keeps running), move it to its own tab.
inline constexpr const char *attach = "daemon.attach";
inline constexpr const char *attachGo = "daemon.attach.go";
inline constexpr const char *attachAll = "daemon.attach.all";
inline constexpr const char *attachAllGo = "daemon.attach.all.go";
inline constexpr const char *attachCancel = "daemon.attach.cancel";
inline constexpr const char *detachPane = "daemon.detach";
inline constexpr const char *movePane = "pane.move-to-tab";
// A terminal of the daemon in the pane that has the focus (replacing its terminal), ending the
// program of a pane, and whole tabs: detach one (the terminals stay, the layout is saved) and
// attach a saved layout again.
inline constexpr const char *attachHere = "daemon.attachhere";
inline constexpr const char *attachHereGo = "daemon.attachhere.go";     // "...:<id>[:force]"
inline constexpr const char *attachHereDo = "daemon.attachhere.do";     // "...:<id>:end|keep"
inline constexpr const char *killPane = "daemon.kill";
inline constexpr const char *killConfirm = "daemon.kill.confirm";
inline constexpr const char *detachTab = "tab.detach";
inline constexpr const char *attachLayout = "layout.attach";            // The list, then "...:<name>".
inline constexpr const char *attachLayoutGo = "layout.attach.go";       // "...:<name>[:force]"
inline constexpr const char *deleteLayout = "layout.delete";
inline constexpr const char *renamePane = "pane.rename"; // The prompt, then "pane.rename:<name>".
} // namespace daemonaction

// What the first terminal says when the app wanted the daemon and could not have it, so it
// is visible in the window and not only on stderr. Plain text, one line per sentence.
inline std::u32string daemonFallbackNote(const std::string &error) {
    std::string clean;
    for (const char c : error) {
        clean += (static_cast<unsigned char>(c) < 32 || c == 127) ? ' ' : c;
    }
    if (clean.size() > 160) {
        clean.resize(160);
    }
    std::string text = "matterm: the background daemon could not be started" +
                       (clean.empty() ? std::string() : " (" + clean + ")") + ".\r\n" +
                       "matterm: terminals run in this window only and end with it. "
                       "--no-daemon hides this note.\r\n";
    std::u32string out;
    for (const char c : text) {
        out += static_cast<char32_t>(static_cast<unsigned char>(c));
    }
    return out;
}

// Entries for the palette's `extra` list. Without a daemon (the default) there
// are none, so nobody is offered an action that cannot work.
inline std::vector<CommandEntry> daemonCommandEntries(bool usingDaemon,
                                                      bool remote = false) {
    if (!usingDaemon) {
        return {};
    }
    std::vector<CommandEntry> entries = {
        {daemonaction::stop,
         "Stop daemon",
         "End the background program that keeps terminals running, and every "
         "program in it",
         std::nullopt,
         {},
         {}},
        {daemonaction::quitKeep,
         "Quit and keep running",
         "Close this window; the programs keep running in the daemon",
         std::nullopt,
         {},
         {}},
        {daemonaction::status,
         "Daemon status",
         "Show how many programs the daemon keeps and who is attached",
         std::nullopt,
         {},
         {}},
    };
    if (remote) {
        entries.push_back({daemonaction::reconnect,
                           "Reconnect",
                           "Start the ssh connection to the other computer "
                           "again and attach its terminals",
                           std::nullopt,
                           {},
                           {}});
    }
    return entries;
}

// What the palette needs to know to offer the sharing entries that make sense
// now.
struct ShareState {
    std::size_t panes = 0;  // Panes in the active tab.
    std::size_t shared = 0; // Of those, shared.
    bool focusedShared = false;
};

inline std::vector<CommandEntry> shareCommandEntries(const ShareState &state) {
    std::vector<CommandEntry> out;
    const auto add =
        [&](const char *id, const char *name, const char *description) {
            out.push_back({id, name, description, std::nullopt, {}, {}});
        };
    if (!state.focusedShared) {
        add(daemonaction::sharePane,
            "Share this terminal",
            "Let a browser on this machine's server take this terminal over");
    }
    else {
        add(daemonaction::sharePaneStop,
            "Stop sharing this terminal",
            "Make this terminal private again");
    }
    if (state.shared < state.panes) {
        add(daemonaction::shareTab,
            "Share this tab",
            "Share every terminal of this tab");
    }
    if (state.shared > 0) {
        add(daemonaction::shareTabStop,
            "Stop sharing this tab",
            "Make every terminal of this tab private again");
    }
    return out;
}

// What the palette offers for the terminals in the daemon, given the window's state.
struct AttachState {
    std::size_t panesInTab = 0;    // Panes in the active tab.
    std::size_t leftover = 0;      // Terminals in the daemon that no window shows.
    bool focusedInDaemon = false;  // The focused pane is a daemon terminal.
    bool tabHasDaemonPane = false; // The active tab shows at least one daemon terminal.
    std::size_t layouts = 0;       // Detached layouts saved.
};

inline std::vector<CommandEntry> attachCommandEntries(const AttachState &state) {
    std::vector<CommandEntry> out;
    const auto add = [&](const char *id, const char *name, const char *description) {
        out.push_back({id, name, description, std::nullopt, {}, {}});
    };
    add(daemonaction::attach, "Attach terminal...",
        "Show a terminal that runs in the daemon in a new tab");
    if (state.focusedInDaemon) {
        add(daemonaction::attachHere, "Attach terminal here...",
            "Show a terminal of the daemon in this pane instead of the one it has");
        add(daemonaction::killPane, "Kill terminal",
            "End the program in this pane and close the pane");
    }
    if (state.tabHasDaemonPane) {
        add(daemonaction::detachTab, "Detach tab (keep terminals)",
            "Take this tab out of the window; its terminals keep running and the layout is saved");
    }
    if (state.layouts > 0) {
        add(daemonaction::attachLayout, "Attach tab...",
            "Open a detached tab again, with its splits and its terminals");
    }
    if (state.leftover > 0) {
        add(daemonaction::attachAll, "Attach all leftover terminals",
            "Open every terminal that no window shows, each in its own tab");
    }
    if (state.focusedInDaemon) {
        add(daemonaction::renamePane, "Rename terminal...",
            "Give this terminal a name that shows when it is attached or taken over");
        add(daemonaction::detachPane, "Detach terminal (keep running)",
            "Close this pane; its program keeps running in the daemon");
        if (state.panesInTab > 1) {
            add(daemonaction::movePane, "Move terminal to new tab",
                "Take this pane out of its tab and show it in a tab of its own");
        }
    }
    return out;
}

// One terminal of the daemon as the list shows it.
struct AttachableSession {
    std::string id;
    std::string cwd;                  // Empty when unknown.
    std::vector<std::string> command; // Empty: a shell waits at its prompt.
    bool attached = false;            // Some window shows it now.
    bool shared = false;
    bool inThisWindow = false;        // This window shows it (not offered).
    bool thisTerminal = false;        // The window itself runs inside it (MATTERM_SESSION): attaching would loop.
    std::string name;                 // Given by the owner (Rename terminal...); empty: none.
};

namespace attachdetail {
inline std::string plain(const std::string &text, std::size_t limit) {
    std::string out;
    for (const char c : text) {
        out += (static_cast<unsigned char>(c) < 32 || c == 127) ? ' ' : c;
    }
    if (out.size() > limit) {
        out.resize(limit - 1);
        out += "~";
    }
    return out;
}
} // namespace attachdetail

// The last two folders of a path, "~/x/y" shortened to "x/y"; "/" stays.
inline std::string shortFolder(const std::string &path) {
    if (path.empty()) return "terminal";
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path + "/") {
        if (c == '/') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        }
        else {
            part += c;
        }
    }
    if (parts.empty()) return "/";
    const std::size_t from = parts.size() > 2 ? parts.size() - 2 : 0;
    std::string out = from > 0 ? ".../" : "";
    for (std::size_t i = from; i < parts.size(); ++i) out += (i > from ? "/" : "") + parts[i];
    return attachdetail::plain(out, 40);
}

// The list items: the folder, and dim at the right what runs and who has it. Terminals this
// window already shows are left out; ones another window shows say so.
inline std::vector<ui::PickList::Item> attachItems(const std::vector<AttachableSession> &sessions,
                                                   const char *action = daemonaction::attachGo) {
    std::vector<ui::PickList::Item> items;
    for (const auto &s : sessions) {
        if (s.inThisWindow) continue;
        std::string command;
        for (const auto &word : s.command) command += (command.empty() ? "" : " ") + word;
        std::string detail = command.empty() ? "shell" : attachdetail::plain(command, 24);
        if (s.thisTerminal) detail += " (this terminal)";
        else if (s.attached) detail += " (in another window)";
        if (s.shared) detail += " (shared)";
        // A named terminal is listed by its name, with the folder after it.
        const std::string label = s.name.empty() ? shortFolder(s.cwd)
                                                 : attachdetail::plain(s.name, 40) + " (" + shortFolder(s.cwd) + ")";
        items.push_back({label, detail, std::string(action) + ":" + s.id});
    }
    return items;
}

// What the owner is told when an attach is refused because the window itself runs in that terminal.
inline const char *thisTerminalNotice() {
    return "you are in that terminal: attaching it would feed it to itself";
}

// What the owner is told when another window took a terminal over.
inline std::string takenOverNotice(bool shared, const std::string &name) {
    std::string text = shared ? "a shared terminal" : "a terminal";
    if (!name.empty()) text += " '" + attachdetail::plain(name, 40) + "'";
    return text + (shared ? " was taken over from another window" : " was taken over by another window");
}

inline std::unique_ptr<ui::ConfirmDialog> takeOverDialog(const std::string &id, const std::string &what,
                                                         const char *action = daemonaction::attachGo) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Take over", std::string(action) + ":" + id + ":force"},
        {"Cancel", daemonaction::attachCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        "Another window shows " + attachdetail::plain(what, 30) + ". Take it over? That window loses it.",
        std::move(buttons), 1, 1);
}

// Before the terminal of a pane is replaced by another: what runs there ("" = an idle shell).
// Three answers; Cancel is the default and what Escape says.
inline std::unique_ptr<ui::ConfirmDialog> replaceTerminalDialog(const std::string &id, const std::string &running) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Replace and end it", std::string(daemonaction::attachHereDo) + ":" + id + ":end"},
        {"Replace, keep it running", std::string(daemonaction::attachHereDo) + ":" + id + ":keep"},
        {"Cancel", daemonaction::attachCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        running.empty() ? std::string("Replace the terminal of this pane?")
                        : "This pane runs " + attachdetail::plain(running, 40) + ". Replace its terminal?",
        std::move(buttons), 2, 2);
}

inline std::unique_ptr<ui::ConfirmDialog> killTerminalDialog(const std::string &running) {
    std::vector<ui::ConfirmDialog::Button> buttons = {{"Kill", daemonaction::killConfirm},
                                                      {"Cancel", daemonaction::attachCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        running.empty() ? std::string("End the shell of this pane and close the pane?")
                        : "End " + attachdetail::plain(running, 40) + " and close the pane?",
        std::move(buttons), 1, 1);
}

// What attaching a saved layout will find, from the layout's terminals and what the daemon holds.
struct LayoutAttachPlan {
    std::size_t panes = 0;       // Panes in the layout.
    std::size_t reused = 0;      // Its terminal is there and free: attached again.
    std::size_t takeOver = 0;    // Its terminal is shown by another window.
    std::size_t fresh = 0;       // No terminal named, or it is gone (or already shown here): a new one.
    std::vector<std::string> use; // Per pane in tree order: the terminal to attach, empty: a new one.
};

inline void collectTerminals(const SavedNode &n, std::vector<const SavedPane *> &out) {
    if (n.leaf) {
        out.push_back(&n.pane);
        return;
    }
    collectTerminals(*n.first, out);
    collectTerminals(*n.second, out);
}

inline LayoutAttachPlan planLayoutAttach(const SavedTab &tab, const std::vector<AttachableSession> &sessions) {
    LayoutAttachPlan plan;
    std::vector<const SavedPane *> panes;
    collectTerminals(*tab.root, panes);
    std::vector<std::string> taken;
    for (const SavedPane *p : panes) {
        ++plan.panes;
        std::string use;
        for (const auto &s : sessions) {
            if (s.id != p->terminal || p->terminal.empty() || s.inThisWindow) continue;
            if (std::find(taken.begin(), taken.end(), s.id) != taken.end()) continue; // Named twice: once.
            use = s.id;
            s.attached ? ++plan.takeOver : ++plan.reused;
            break;
        }
        if (use.empty()) {
            ++plan.fresh;
        }
        else {
            taken.push_back(use);
        }
        plan.use.push_back(std::move(use));
    }
    return plan;
}

inline std::unique_ptr<ui::ConfirmDialog> takeOverLayoutDialog(const std::string &name, std::size_t count) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Take over", std::string(daemonaction::attachLayoutGo) + ":" + name + ":force"},
        {"Cancel", daemonaction::attachCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        std::to_string(count) + (count == 1 ? " terminal of this layout is" : " terminals of this layout are") +
            " shown by another window. Take " + (count == 1 ? "it" : "them") + " over?",
        std::move(buttons), 1, 1);
}

inline std::unique_ptr<ui::ConfirmDialog> attachAllDialog(std::size_t count) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Attach", daemonaction::attachAllGo}, {"Cancel", daemonaction::attachCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        "Open " + std::to_string(count) + (count == 1 ? " terminal" : " terminals") +
            " that no window shows, each in a new tab?",
        std::move(buttons), 1, 0);
}

inline std::string plural(std::size_t n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The question before the daemon is stopped: says what will end. The safe
// button (Cancel) is the default and what Escape answers.
// `names`: what the terminals run, when the daemon could say ("vim, make").
inline std::string stopDaemonQuestion(
    std::size_t programs, const std::vector<std::string> &names = {}) {
    if (programs == 0) {
        return "Stop the daemon? No programs are running in it.";
    }
    std::string list;
    for (std::size_t i = 0; i < names.size(); ++i) {
        list += (i == 0 ? " (" : ", ") + names[i];
    }
    if (!list.empty()) {
        list += ")";
    }
    return "Stop the daemon? " + plural(programs, "program", "programs") +
           " running in it" + list + " will end, also those no window shows.";
}

inline std::unique_ptr<ui::ConfirmDialog> stopDaemonDialog(
    std::size_t programs, const std::vector<std::string> &names = {}) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Stop daemon", daemonaction::stopConfirm},
        {"Cancel", daemonaction::stopCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        stopDaemonQuestion(programs, names), std::move(buttons), 1, 1);
}

struct DaemonStatus {
    bool connected = false;
    std::string socketPath;
    std::size_t programs = 0; // Terminals the daemon holds.
    std::size_t attached = 0; // Of those, shown by some window.
};

// One line for the status bar or a notice.
inline std::string formatDaemonStatus(const DaemonStatus &status) {
    if (!status.connected) {
        return "daemon: not running" + (status.socketPath.empty()
                                            ? std::string()
                                            : " (" + status.socketPath + ")");
    }
    const std::size_t detached = status.programs > status.attached
                                     ? status.programs - status.attached
                                     : 0;
    std::string text =
        "daemon: " + plural(status.programs, "program", "programs");
    if (status.programs > 0) {
        text += ", " + std::to_string(status.attached) + " shown";
        if (detached > 0) {
            text +=
                ", " + std::to_string(detached) + " running without a window";
        }
    }
    return text;
}

} // namespace frontend
