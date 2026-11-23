#pragma once

// After "Open session" (docs/tasks/restore-programs.md): one question, "Start 3
// programs again?", for the panes that had a program running. Pure: which panes
// are counted, the question and the lines of the list, and what is typed for
// "Start them". Nothing is started without the answer.
#include "closeprompt.h"
#include "daemoncommands.h"
#include "paneprobe.h"
#include "sessionsnapshot.h"
#include "workspace.h"

#include <cstddef>
#include <string>
#include <vector>

namespace frontend {

// A restored pane with what the question needs to know about it.
struct ProgramCandidate {
    PaneId pane = 0;
    SavedPane saved;
    bool folderGone = false; // Its folder is gone: left out (the prompt-line
                             // offer still comes).
    bool noTerminal =
        false; // Its terminal could not be created or is not live.
};

struct OfferedProgram {
    PaneId pane = 0;
    SavedPane saved;
    std::string
        command;      // commandLine(): shell-quoted, only for a plainCommand().
    std::string line; // For the list: the short folder and the command.
};

inline std::vector<OfferedProgram> offeredPrograms(
    const std::vector<ProgramCandidate> &candidates) {
    std::vector<OfferedProgram> out;
    for (const auto &c : candidates) {
        if (c.folderGone || c.noTerminal)
            continue;
        std::string command = commandLine(
            c.saved.command); // Empty: nothing was running, or refused.
        if (command.empty())
            continue;
        OfferedProgram p;
        p.pane = c.pane;
        p.saved = c.saved;
        p.line = (c.saved.cwd.empty() ? std::string()
                                      : shortFolder(c.saved.cwd) + "  ") +
                 CloseRequest::shown(command);
        p.command = std::move(command);
        out.push_back(std::move(p));
    }
    return out;
}

inline std::string startQuestion(std::size_t count) {
    return "Start " + std::to_string(count) +
           (count == 1 ? " program" : " programs") + " again?";
}

// What "Start them" types into the pane's shell: the command and Enter.
inline std::string startInput(const OfferedProgram &program) {
    return program.command + "\r";
}

// The action ids of the two answers.
namespace restoreaction {
inline constexpr const char *start = "restore.programs.start";
inline constexpr const char *skip =
    "restore.programs.skip"; // Not now: the prompt-line offers.
} // namespace restoreaction

} // namespace frontend
