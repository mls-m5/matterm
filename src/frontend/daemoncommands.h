#pragma once

// The palette actions that belong to the local daemon (docs/tasks/daemon.md):
// stop it, quit the window but leave the programs running, show what it holds.
// Only what the front end needs to offer them: the entries, the confirmation
// text, the status text. No sockets here; the application does the work when
// the action comes back.
#include "commandregistry.h"
#include "ui/dialogs.h"

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
} // namespace daemonaction

// Entries for the palette's `extra` list. Without a daemon (the default) there
// are none, so nobody is offered an action that cannot work.
inline std::vector<CommandEntry> daemonCommandEntries(bool usingDaemon) {
    if (!usingDaemon) {
        return {};
    }
    return {
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
}

inline std::string plural(std::size_t n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The question before the daemon is stopped: says what will end. The safe
// button (Cancel) is the default and what Escape answers.
inline std::string stopDaemonQuestion(std::size_t programs) {
    if (programs == 0) {
        return "Stop the daemon? No programs are running in it.";
    }
    return "Stop the daemon? " + plural(programs, "program", "programs") +
           " running in it will end, also those no window shows.";
}

inline std::unique_ptr<ui::ConfirmDialog> stopDaemonDialog(
    std::size_t programs) {
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Stop daemon", daemonaction::stopConfirm},
        {"Cancel", daemonaction::stopCancel}};
    return std::make_unique<ui::ConfirmDialog>(
        stopDaemonQuestion(programs), std::move(buttons), 1, 1);
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
