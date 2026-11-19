#pragma once

#include "keys.h"
#include "types.h"
#include "virtualfs.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fakeos {

// Everything a command, a program or an easter egg may read or change: the
// virtual file system, the shell's state, the clock and the discoveries made so
// far. There are no globals; one World belongs to one System.
struct World {
    VirtualFs fs;
    std::string user = "guest";
    std::string hostname = "matterm";
    std::string cwd = "/home/guest";
    std::map<std::string, std::string> env;
    int lastStatus = 0;
    std::vector<std::string> history;
    int columns = 80, rows = 24;
    Capabilities caps;
    // The terminal's default background: animations fade to it, so leaving the
    // alternate screen shows no jump.
    Rgb background{30, 30, 30};
    Rng rng;
    Time now{0};               // Time since the system started.
    std::int64_t bootUnix = 0; // Unix seconds at start; see unixNow().
    // Names of discoveries (easter eggs found, programs unlocked).
    std::set<std::string> flags;
    // Set by commands, handled by the System after the command line ran.
    std::optional<int> exitRequest;
    bool resetRequest = false;
    struct Launch {
        std::string name;
        std::vector<std::string> args;
        std::string input; // Text for the program (a pager's file), if any.
    };
    // The text handed to the program that is starting (read in its begin()).
    std::string programInput;
    std::optional<Launch> launch;

    std::int64_t unixNow() const {
        return bootUnix +
               std::chrono::duration_cast<std::chrono::seconds>(now).count();
    }
    std::string home() const {
        const auto it = env.find("HOME");
        return it == env.end() ? "/" : it->second;
    }
    std::string path(const std::string &relative) const {
        return VirtualFs::normalize(cwd, relative);
    }
};

// Fills `world` with the initial virtual system for its user and clock.
void seedWorld(World &world);

} // namespace fakeos
