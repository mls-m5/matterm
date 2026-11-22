#pragma once

// Palette actions for the saved sessions (docs/TABS_PLAN.md section 9) that need more than a
// name: deleting one. The application does the work when the action comes back.
#include "commandregistry.h"
#include "ui/dialogs.h"

#include <memory>
#include <string>
#include <vector>

namespace frontend {

namespace sessionaction {
inline constexpr const char *remove = "session.delete";               // The list; then "session.delete:<name>".
inline constexpr const char *removeConfirm = "session.delete.confirm"; // "...confirm:<name>"
inline constexpr const char *removeCancel = "session.delete.cancel";
} // namespace sessionaction

inline std::vector<CommandEntry> sessionCommandEntries() {
    return {{sessionaction::remove, "Delete session...", "Remove a saved session (not the running programs)",
             std::nullopt, {}, {}}};
}

// The question before a saved session is removed. Cancel is the default and what Escape answers.
// `inUse`: it is the session this window saves into, which says so.
inline std::unique_ptr<ui::ConfirmDialog> deleteSessionDialog(const std::string &name, bool inUse) {
    std::string shown;
    for (const char c : name) shown += (static_cast<unsigned char>(c) < 32 || c == 127) ? '?' : c;
    std::string question = "Delete the saved session '" + shown + "'?";
    question += inUse ? " This window uses it: it is not saved again until you name a tab or save it."
                      : " The programs now running are not affected.";
    std::vector<ui::ConfirmDialog::Button> buttons = {
        {"Delete", std::string(sessionaction::removeConfirm) + ":" + name},
        {"Cancel", sessionaction::removeCancel}};
    return std::make_unique<ui::ConfirmDialog>(std::move(question), std::move(buttons), 1, 1);
}

} // namespace frontend
