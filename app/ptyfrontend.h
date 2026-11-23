#pragma once

#include <string>
#include <vector>

// The local daemon (docs/tasks/daemon.md): panes live in matterm-daemon instead of this process.
struct DaemonOptions {
    bool enabled = false;
    std::string socketPath; // Empty: the default socket.
    bool required = false;  // --daemon given: fail instead of running without it.
    bool snapScroll = false; // --snap-scroll: scrolling comes to rest on whole rows.
    // --daemon-host: the daemon of another computer through an ssh tunnel (docs/REMOTE_DAEMON.md).
    std::string host;
    std::vector<std::string> sshArguments; // --ssh-arg, before the ones the tunnel adds.
    bool installAlways = false; // --daemon-install: use our own copy of matterm-daemon there.
    bool assumeYes = false;     // --yes: copy it without asking.
    std::string tabActivity = "icon"; // --tab-activity icon|pulse|off (--no-tab-pulse = off)
    bool borderless = true;           // Our own frame (the default; --system-frame turns it off, docs/TABS_PLAN.md section 16)
    // matterm --attach-tty / matmux: start a terminal at once (--new [--name N] [-- COMMAND...]).
    // What the client tells the user to run to come back to a terminal it left running.
    std::string attachProgram = "matterm --attach-tty";
    // matmux shows a terminal by passing its output through (the host's own scrollback); --rendered (and
    // matterm --attach-tty) draws it cell by cell instead.
    bool rendered = true;
    // matmux without arguments: the list with New terminal chosen, or a new terminal at once when none runs.
    bool attachDefault = false;
    bool attachNew = false;
    std::string attachName;
    std::vector<std::string> attachCommand;
    bool layoutSmoke = false;         // --pty-layout-smoke-test (hidden): Detach tab, then Attach tab... is offered
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
