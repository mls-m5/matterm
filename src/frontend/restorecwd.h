#pragma once

// The folder a restored or new terminal is asked to start in
// (docs/REMOTE_DAEMON.md): a saved folder of a daemon on this computer is
// checked here; one of a daemon on another computer means nothing here, so it
// goes to the daemon as it is, which looks at it on its own computer and starts
// in its home folder when it is missing.
#include <string>

namespace frontend {

// `existsHere`: the folder exists on this computer (only asked for a local
// daemon).
inline std::string cwdToRequest(const std::string &saved,
                                bool remoteDaemon,
                                bool existsHere) {
    if (saved.empty()) {
        return {};
    }
    return remoteDaemon || existsHere ? saved : std::string();
}

// True when a terminal that was asked to start in `saved` is somewhere else:
// the note "restored without its folder". `actual` is where it runs, empty when
// that is not known (no note then).
inline bool restoredElsewhere(const std::string &saved,
                              const std::string &actual) {
    return !saved.empty() && !actual.empty() && actual != saved;
}

} // namespace frontend
