#pragma once

// "Attach terminal..." and "Attach tab..." in the window: when the tab that was
// in front is an empty shell (nothing was typed, nothing runs, nothing scrolled
// by), it is closed after the attach worked, and its fresh terminal ends in the
// daemon: nothing is lost (docs/handover/features/agent3-close-empty-shell.md).
// This is the rule, with no window in it.
#include <cstddef>
#include <cstdint>

namespace frontend {

struct ShellFacts {
    std::size_t panesInTab = 0; // The tab has exactly one pane.
    std::uint64_t inputBytes =
        0; // Bytes the user sent to the terminal since it started (keys, paste,
           // mouse reports); answers of the terminal itself do not count.
    bool probeKnown = false; // What runs there was read (a daemon that did not
                             // answer is not known).
    bool busy = false; // A program other than the shell runs in the foreground.
    std::size_t scrollbackRows =
        0; // Lines that have scrolled out of the screen.
    bool selecting =
        false; // The owner is in copy mode or has a selection there.
};

inline bool isEmptyShell(const ShellFacts &facts) {
    return facts.panesInTab == 1 && facts.inputBytes == 0 && facts.probeKnown &&
           !facts.busy && facts.scrollbackRows == 0 && !facts.selecting;
}

} // namespace frontend
