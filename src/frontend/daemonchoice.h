#pragma once

// Which daemon to talk to (src/daemon/version.h, src/daemon/socketpath.h,
// docs/REMOTE_DAEMON.md "Versions and several daemons"). A daemon is identified
// by its socket and several can run side by side. Without --daemon-socket the
// compatible ones in the socket folder are the candidates: one is used, of
// several the newest (after a question), and if none is compatible a new daemon
// is offered, on a socket named after its version, next to the old ones (they
// keep their terminals). With --daemon-socket that daemon is used, or the offer
// is made next to it. The tools are injected so that tests need no daemon.
#include "daemonclient.h"
#include "socketpath.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace frontend {

struct DaemonChoiceTools {
    // The sockets in the default folder, newest version first
    // (mtd::daemonSocketsIn).
    std::function<std::vector<std::string>()> list;
    std::function<std::optional<DaemonInfo>(const std::string &socket)> probe;
    // Starts a daemon on exactly this socket (empty: done, else the reason).
    std::function<std::string(const std::string &socket)> start;
    // A yes/no question to the user; false when nobody can answer.
    std::function<bool(const std::string &question)> ask;
    // The socket a new daemon of this build gets when none is chosen.
    std::string newSocket;
};

struct DaemonChoice {
    std::string socket; // To connect to; empty when `error` is set.
    std::string
        note; // Worth showing (compatible, but not identical); may be empty.
    std::string error; // Why no daemon could be chosen.
};

inline std::string describeVersions(const mtd::Version &daemon) {
    return "the daemon runs control version " + mtd::toString(daemon) +
           ", this program speaks " + mtd::toString(mtd::ownVersion());
}

inline std::string terminalsOf(const DaemonInfo &info) {
    return info.status ? std::to_string(info.status->sessions) + " terminals"
                       : std::string("its terminals");
}

struct DaemonChoiceRequest {
    std::string
        socket; // --daemon-socket; empty: choose among the daemons found.
    bool offerNew = true;     // False where a second daemon cannot be offered
                              // (another computer).
    bool startMissing = true; // Start a daemon when none runs.
};

namespace detail {
inline bool older(const DaemonInfo &a,
                  const DaemonInfo &b) { // a is older than b
    return std::tie(a.version.format, a.version.breaking, a.version.additive) <
           std::tie(b.version.format, b.version.breaking, b.version.additive);
}

inline DaemonChoice startNew(const std::string &socket,
                             const std::string &incompatibleNote,
                             const DaemonChoiceRequest &request,
                             const DaemonChoiceTools &tools) {
    DaemonChoice choice;
    if (!request.startMissing) {
        choice.socket =
            socket; // Nothing started; the caller says "not running".
        return choice;
    }
    if (!incompatibleNote.empty() && !request.offerNew) {
        choice.error = incompatibleNote;
        return choice;
    }
    if (!incompatibleNote.empty() &&
        (!tools.ask ||
         !tools.ask(incompatibleNote + " Start a new daemon next to it (" +
                    socket + ")?"))) {
        choice.error = incompatibleNote + " Not starting a new daemon.";
        return choice;
    }
    if (auto error = tools.start(socket); !error.empty()) {
        choice.error = "cannot start a new daemon on " + socket + ": " + error;
        return choice;
    }
    choice.socket = socket;
    if (!incompatibleNote.empty()) {
        choice.note =
            "started a new daemon on " + socket + " next to the old one";
    }
    return choice;
}
} // namespace detail

inline DaemonChoice chooseDaemon(const DaemonChoiceRequest &request,
                                 const DaemonChoiceTools &tools) {
    DaemonChoice choice;
    const auto mine = mtd::ownVersion();
    const std::string stopHint =
        " The old one keeps running; 'matterm --daemon-list' shows the daemons "
        "and 'matterm --daemon-stop' ends one.";
    const auto note = [&](const DaemonInfo &info, const std::string &socket) {
        return "the daemon on " + socket + " is " +
               mtd::toString(info.version) + ", this program " +
               mtd::toString(mine) + " (compatible)";
    };

    if (!request.socket.empty()) {
        const auto info = tools.probe(request.socket);
        if (!info) {
            return detail::startNew(request.socket, {}, request, tools);
        }
        const auto relation = mtd::compatibility(mine, info->version);
        if (relation == mtd::Compatibility::Same) {
            choice.socket = request.socket;
        }
        else if (relation != mtd::Compatibility::Incompatible) {
            choice.socket = request.socket;
            choice.note = note(*info, request.socket);
        }
        else {
            const std::string sibling = mtd::versionedSiblingSocket(
                request.socket, mine.format, mine.breaking, mine.additive);
            if (const auto other = tools.probe(sibling);
                other && mtd::compatibility(mine, other->version) !=
                             mtd::Compatibility::Incompatible) {
                choice.socket = sibling;
                choice.note =
                    "the daemon on " + request.socket + " is " +
                    mtd::toString(info->version) +
                    " and cannot work with this program; using the one on " +
                    sibling;
                return choice;
            }
            return detail::startNew(
                sibling,
                describeVersions(info->version) +
                    ", and they cannot work together. It runs " +
                    terminalsOf(*info) + "." + stopHint,
                request,
                tools);
        }
        return choice;
    }

    std::vector<std::pair<std::string, DaemonInfo>> compatible, incompatible;
    for (const auto &socket : tools.list()) {
        if (const auto info = tools.probe(socket)) {
            (mtd::compatibility(mine, info->version) ==
                     mtd::Compatibility::Incompatible
                 ? incompatible
                 : compatible)
                .emplace_back(socket, *info);
        }
    }
    if (compatible.empty()) {
        std::string why;
        if (!incompatible.empty()) {
            const auto &old = incompatible.front();
            why = describeVersions(old.second.version) +
                  ", and they cannot work together (" + old.first + " runs " +
                  terminalsOf(old.second) + ")." + stopHint;
        }
        return detail::startNew(tools.newSocket, why, request, tools);
    }
    std::stable_sort(
        compatible.begin(), compatible.end(), [](const auto &a, const auto &b) {
            return detail::older(b.second, a.second);
        });
    const auto &best = compatible.front();
    choice.socket = best.first;
    if (mtd::compatibility(mine, best.second.version) !=
        mtd::Compatibility::Same) {
        choice.note = note(best.second, best.first);
    }
    if (compatible.size() > 1) {
        std::string list;
        for (const auto &c : compatible) {
            list += "\n  " + c.first + " (" + mtd::toString(c.second.version) +
                    ", " + terminalsOf(c.second) + ")";
        }
        if (!tools.ask || !tools.ask("Several daemons fit:" + list +
                                     "\nUse the newest, " + best.first + "?")) {
            choice.socket.clear();
            choice.note.clear();
            choice.error = "several daemons fit:" + list +
                           "\nname the one to use with --daemon-socket";
        }
    }
    return choice;
}

} // namespace frontend
