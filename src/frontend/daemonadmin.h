#pragma once

// --daemon-list and --daemon-stop (docs/REMOTE_DAEMON.md "Seeing and stopping
// daemons"): find the daemons of this user, say what each is, and end one on
// request. Stopping does not depend on compatible protocols: a daemon that
// answers the frozen handshake is asked to shut down (Shutdown is part of it);
// one that does not is found by the pid of the process behind the socket
// (SO_PEERCRED) and sent SIGTERM, which the daemon handles by ending cleanly.
// The tools are injected so that the logic is tested without processes.
#include "daemonclient.h"
#include "socketpath.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <functional>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace frontend {

struct PeerCredentials {
    std::uint32_t pid = 0;
    std::uint32_t uid = 0;
};

// Who listens on a Unix socket, from the kernel (no protocol involved). nullopt
// when nothing accepts a connection there.
inline std::optional<PeerCredentials> peerCredentials(
    const std::string &socketPath) {
    if (socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
    std::optional<PeerCredentials> result;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) ==
        0) {
        ucred credentials{};
        socklen_t length = sizeof credentials;
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) ==
            0) {
            result = PeerCredentials{std::uint32_t(credentials.pid),
                                     std::uint32_t(credentials.uid)};
        }
    }
    ::close(fd);
    return result;
}

struct DaemonEntry {
    std::string socket;
    std::optional<DaemonInfo>
        info;              // Empty when it did not answer the handshake.
    std::uint32_t pid = 0; // From the Welcome, else from the socket; 0 unknown.
    std::string problem;   // Set when `info` is empty.
    std::optional<mtd::Activity>
        activity; // Empty for a daemon older than 1.3.2.
};

using DaemonProbe =
    std::function<std::optional<DaemonInfo>(const std::string &socket)>;

inline DaemonProbe realProbe() {
    return [](const std::string &socket) {
        std::string error;
        return probeDaemon(socket, error, 1500);
    };
}

// Where daemons of this user keep their sockets.
inline std::vector<std::string> daemonSocketDirs() {
    std::vector<std::string> dirs{mtd::socketDirectory()};
    const std::string fallback = "/tmp/matterm-" + std::to_string(::geteuid());
    if (fallback != dirs.front()) {
        dirs.push_back(fallback);
    }
    return dirs;
}

// The *.sock files of the folders (not the .lock files), sorted.
inline std::vector<std::string> findDaemonSockets(
    const std::vector<std::string> &dirs) {
    std::vector<std::string> found;
    for (const auto &dir : dirs) {
        DIR *handle = ::opendir(dir.c_str());
        if (!handle) {
            continue;
        }
        while (const dirent *entry = ::readdir(handle)) {
            const std::string name = entry->d_name;
            if (name.size() > 5 &&
                name.compare(name.size() - 5, 5, ".sock") == 0) {
                found.push_back(dir + "/" + name);
            }
        }
        ::closedir(handle);
    }
    std::sort(found.begin(), found.end());
    return found;
}

inline std::optional<mtd::Activity> realActivity(const std::string &socket) {
    std::string error;
    const auto reply = askDaemon(socket, mtd::GetActivity{}, error, 1500);
    if (reply) {
        if (const auto *activity = std::get_if<mtd::Activity>(&*reply)) {
            return *activity;
        }
    }
    return std::nullopt;
}

inline std::vector<DaemonEntry> listDaemons(
    const std::vector<std::string> &sockets,
    const DaemonProbe &probe,
    const std::function<std::optional<PeerCredentials>(const std::string &)>
        &peer = peerCredentials,
    const std::function<std::optional<mtd::Activity>(const std::string &)>
        &activity = realActivity) {
    std::vector<DaemonEntry> entries;
    for (const auto &socket : sockets) {
        DaemonEntry entry;
        entry.socket = socket;
        entry.info = probe(socket);
        if (entry.info) {
            entry.pid = entry.info->welcome.pid;
            if (entry.info->version.additive >= 2) {
                entry.activity = activity(socket);
            }
        }
        else if (const auto who = peer(socket)) {
            entry.pid = who->pid;
            entry.problem = "does not answer the handshake (too old, or not a "
                            "matterm daemon)";
        }
        else {
            entry.problem = "nothing listens (a left-over file)";
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

// "less than a minute", "5 min", "3 h", "2 days".
inline std::string formatAge(std::uint32_t seconds) {
    if (seconds < 60) {
        return "less than a minute";
    }
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + " min";
    }
    if (seconds < 48u * 3600) {
        return std::to_string(seconds / 3600) + " h";
    }
    return std::to_string(seconds / 86400) + " days";
}

inline std::string formatDaemonList(const std::vector<DaemonEntry> &entries) {
    if (entries.empty()) {
        return "No daemon found.\n";
    }
    std::string out;
    for (const auto &e : entries) {
        out += e.socket + "\n    ";
        if (e.pid != 0) {
            out += "pid " + std::to_string(e.pid) + ", ";
        }
        if (e.info) {
            out += "version " + mtd::toString(e.info->version);
            if (e.info->status) {
                out += ", " + std::to_string(e.info->status->sessions) +
                       " terminals";
                if (e.info->status->attached != 0) {
                    out += " (" + std::to_string(e.info->status->attached) +
                           " in use)";
                }
            }
            if (e.activity) {
                out += e.activity->attachedNow
                           ? ", a client is attached"
                           : ", last client " +
                                 formatAge(e.activity->secondsSinceClient) +
                                 " ago";
            }
            if (mtd::compatibility(mtd::ownVersion(), e.info->version) ==
                mtd::Compatibility::Incompatible) {
                out += ", cannot work with this program";
            }
        }
        else {
            out += e.problem;
        }
        out += "\n";
    }
    return out;
}

// One line for the start of the window or --attach-tty when other daemons than
// the one in use run (or one is older): "2 other daemons run (1 old):
// --daemon-list". Empty when there are none.
inline std::string strayNotice(const std::vector<DaemonEntry> &entries,
                               const std::string &used,
                               const mtd::Version &mine) {
    int others = 0, old = 0;
    for (const auto &e : entries) {
        if (e.socket == used || !e.info) {
            continue;
        }
        ++others;
        if (std::make_tuple(e.info->version.format,
                            e.info->version.breaking,
                            e.info->version.additive) <
            std::make_tuple(mine.format, mine.breaking, mine.additive)) {
            ++old;
        }
    }
    if (others == 0) {
        return {};
    }
    return std::to_string(others) +
           (others == 1 ? " other daemon runs" : " other daemons run") +
           (old > 0 ? " (" + std::to_string(old) + " old)" : std::string()) +
           ": --daemon-list";
}

struct DaemonStopTools {
    DaemonProbe probe;
    std::function<bool(const std::string &question)> ask;
    // Shutdown{count}; nullopt when no answer came.
    std::function<std::optional<mtd::ShutdownReply>(const std::string &socket,
                                                    std::uint32_t count)>
        shutdown;
    std::function<std::optional<PeerCredentials>(const std::string &socket)>
        peer;
    std::function<bool(std::uint32_t pid)> terminate; // SIGTERM.
    std::function<bool(const std::string &socket)>
        gone; // Waits a little for the socket to go.
    // The programs that run in its terminals, when known (may be empty).
    std::function<std::vector<std::string>(const std::string &socket)> programs;
};

struct StopResult {
    bool stopped = false;
    std::string message;
};

inline StopResult stopDaemon(const std::string &socket,
                             const DaemonStopTools &tools) {
    StopResult result;
    const auto info = tools.probe(socket);
    std::optional<PeerCredentials> peer;
    if (info) {
        std::string count =
            info->status ? std::to_string(info->status->sessions) + " terminals"
                         : std::string("its terminals");
        if (tools.programs) {
            const auto programs = tools.programs(socket);
            for (std::size_t i = 0; i < programs.size(); ++i) {
                count += (i == 0 ? " (running: " : ", ") + programs[i];
            }
            if (!programs.empty()) {
                count += ")";
            }
        }
        if (!tools.ask("Stop the daemon on " + socket + " (pid " +
                       std::to_string(info->welcome.pid) + ", version " +
                       mtd::toString(info->version) + ")? This ends " + count +
                       ".")) {
            result.message = "Not stopped.";
            return result;
        }
        const std::uint32_t expected =
            info->status ? info->status->sessions : 0;
        if (const auto reply = tools.shutdown(socket, expected)) {
            if (!reply->done) {
                result.message = "The daemon now runs " +
                                 std::to_string(reply->sessions) +
                                 " terminals, not " + std::to_string(expected) +
                                 "; nothing was stopped.";
                return result;
            }
            result.stopped = true;
            result.message =
                tools.gone(socket)
                    ? "The daemon is stopped."
                    : "The daemon was told to stop; it is still shutting down.";
            return result;
        }
        peer = tools.peer(
            socket); // No answer to Shutdown: the pid is all that is left.
        if (!peer) {
            result.message =
                "The daemon did not answer and its process could not be found.";
            return result;
        }
    }
    else {
        peer = tools.peer(socket);
        if (!peer) {
            result.message = "No daemon answers on " + socket + ".";
            return result;
        }
        if (peer->uid == ::geteuid() &&
            !tools.ask(
                "The process on " + socket + " (pid " +
                std::to_string(peer->pid) +
                ") does not answer the handshake (too old, or not a matterm "
                "daemon). Send it SIGTERM? "
                "The terminals it keeps (their number is unknown) end.")) {
            result.message = "Not stopped.";
            return result;
        }
    }
    if (peer->uid != ::geteuid()) {
        result.message = "The process on " + socket +
                         " belongs to another user; not touching it.";
        return result;
    }
    if (!tools.terminate(peer->pid)) {
        result.message =
            "Could not signal pid " + std::to_string(peer->pid) + ".";
        return result;
    }
    result.stopped = true;
    result.message =
        tools.gone(socket)
            ? "The process is stopped (SIGTERM)."
            : "The process was sent SIGTERM; it is still shutting down.";
    return result;
}

// The foreground programs that are not a shell, as the daemon reports them
// (version 2+); at most 5.
inline std::vector<std::string> daemonProgramNames(const std::string &socket) {
    // The foreground programs that are not a shell, as the daemon reports them
    // (version 2+).
    std::vector<std::string> names;
    std::string error;
    const auto list = askDaemon(socket, mtd::ListSessions{}, error, 1500);
    const auto *sessions =
        list ? std::get_if<mtd::SessionList>(&*list) : nullptr;
    for (std::size_t i = 0;
         sessions && i < sessions->sessions.size() && names.size() < 5;
         ++i) {
        const auto detail = askDaemon(
            socket, mtd::GetSessionInfo{sessions->sessions[i].id}, error, 1000);
        const auto *info =
            detail ? std::get_if<mtd::SessionInfo>(&*detail) : nullptr;
        if (info && info->busy && !info->command.empty()) {
            auto name = info->command.front();
            name = name.substr(
                name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
            if (!name.empty() &&
                std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

inline DaemonStopTools realStopTools(
    std::function<bool(const std::string &)> ask) {
    DaemonStopTools tools;
    tools.probe = realProbe();
    tools.ask = std::move(ask);
    tools.shutdown =
        [](const std::string &socket,
           std::uint32_t count) -> std::optional<mtd::ShutdownReply> {
        std::string error;
        const auto reply = askDaemon(socket, mtd::Shutdown{count}, error, 3000);
        if (reply) {
            if (const auto *done = std::get_if<mtd::ShutdownReply>(&*reply)) {
                return *done;
            }
        }
        return std::nullopt;
    };
    tools.programs = daemonProgramNames;
    tools.peer = [](const std::string &socket) {
        return peerCredentials(socket);
    };
    tools.terminate = [](std::uint32_t pid) {
        return ::kill(pid_t(pid), SIGTERM) == 0;
    };
    tools.gone = [](const std::string &socket) {
        for (int i = 0; i < 60; ++i) {
            if (!daemonRunning(socket)) {
                return true;
            }
            ::usleep(50000);
        }
        return false;
    };
    return tools;
}

} // namespace frontend
