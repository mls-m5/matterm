#pragma once

#include <string>
#include <vector>

// The local daemon (docs/tasks/daemon.md): panes live in matterm-daemon instead of this process.
struct DaemonOptions {
    bool enabled = false;
    std::string socketPath; // Empty: the default socket.
};

// Native entry-point adapter; owns SDL lifecycle after starting the PTY.
int runPtyFrontend(const char *font,
                   int fontSize,
                   const std::vector<std::string> &arguments,
                   const std::string &term,
                   bool smokeTest,
                   const char *recordPath,
                   bool smoothCursor = false,
                   bool tabsSmoke = false,
                   const DaemonOptions &daemon = {});
