#pragma once

// Closing something that still runs a program (docs/TABS_PLAN.md section 9): the
// question the dialog asks, as data. A pane is "busy" when its foreground process is
// not the shell (src/frontend/paneprobe.h). The dialog itself comes from the overlay
// layer; the model only says what is at stake and takes the answer.
#include "tabtypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace frontend {

enum class CloseScope { Pane, Tab, Window };

// Close or cancel. A third answer, "run in background" (keep the program alive in a
// local session daemon, docs/TABS_PLAN.md section 9 point 5), is future work: add it
// here and offer it where CloseRequest::backgroundAvailable is true.
enum class CloseAnswer { Close, Cancel };

struct RunningProgram {
    PaneId pane = 0;
    std::string text; // Command line as shown to the owner, already sanitized.
};

struct CloseRequest {
    CloseScope scope = CloseScope::Pane;
    TabId tab = 0;   // Scope Tab: the tab; Scope Pane: the active tab at the time.
    PaneId pane = 0; // Scope Pane: the pane.
    std::vector<RunningProgram> running;
    bool backgroundAvailable = false; // Hook for the future third answer; always false now.

    static constexpr std::size_t kMaxShown = 3;       // Programs named in the question.
    static constexpr std::size_t kMaxShownLength = 60; // Characters of one command line.

    // One line of a command for a dialog: control characters shown as '?', long text cut with
    // "...". A program name read from /proc is not trusted to be printable.
    static std::string shown(const std::string &text) {
        std::string out;
        for (const char ch : text) {
            const auto c = static_cast<unsigned char>(ch);
            if (out.size() >= kMaxShownLength) {
                out += "...";
                break;
            }
            out += (c < 0x20 || c == 0x7f) ? '?' : ch;
        }
        return out;
    }

    // The question, for example "Close this tab? vim notes.txt is still running."
    std::string question() const {
        const char *what = scope == CloseScope::Pane ? "pane" : scope == CloseScope::Tab ? "tab" : "window";
        std::string q = std::string("Close this ") + what + "? ";
        for (std::size_t i = 0; i < running.size() && i < kMaxShown; ++i) {
            if (i) q += ", ";
            q += running[i].text.empty() ? "A program" : shown(running[i].text);
        }
        if (running.size() > kMaxShown) q += " and " + std::to_string(running.size() - kMaxShown) + " more";
        q += running.size() == 1 ? " is still running." : " are still running.";
        return q;
    }
};

} // namespace frontend
