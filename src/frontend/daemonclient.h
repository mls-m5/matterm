#pragma once

// The native app's side of the local daemon that is not a terminal: finding it,
// starting it when it is missing, and the short question-and-answer on its
// control connection (status, shutdown; docs/PROTOCOL.md "Local daemon"). The
// calls block for a short, bounded time and are for user actions (a palette
// entry), not for every frame.
#include "control.h"
#include "unixchannel.h"
#include "version.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace frontend {

// One control conversation: ControlHello, `request`, and the reply that follows
// the ControlWelcome. nullopt (with `error` set) when nothing could be asked or
// answered in time.
inline std::optional<mtd::ControlReply> askDaemon(
    const std::string &socketPath,
    const mtd::ControlRequest &request,
    std::string &error,
    int timeoutMs = 2000,
    mtd::ControlWelcome *welcomeOut = nullptr) {
    try {
        UnixSocketChannel channel(socketPath);
        std::optional<mtd::ControlReply> answer;
        bool welcomed = false;
        bool closed = false;
        MessageChannel::Events events;
        events.opened = [&] {
            std::string hello;
            mtd::encode(mtd::ControlRequest(mtd::ControlHello{}), hello);
            channel.send(hello);
            std::string body;
            mtd::encode(request, body);
            channel.send(body);
        };
        events.message = [&](std::string_view bytes) {
            const auto reply = mtd::decodeReply(bytes);
            if (!reply) {
                error = "unreadable reply from the daemon";
                closed = true;
            }
            else if (!welcomed &&
                     std::holds_alternative<mtd::ControlWelcome>(*reply)) {
                welcomed = true;
                if (welcomeOut) {
                    *welcomeOut = std::get<mtd::ControlWelcome>(*reply);
                }
            }
            else {
                answer = *reply;
            }
        };
        events.closed = [&](int) { closed = true; };
        channel.setEvents(std::move(events));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (!answer && !closed) {
            channel.poll();
            if (answer || closed) {
                break;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                error = "the daemon did not answer";
                channel.close(1000);
                return std::nullopt;
            }
            pollfd p{channel.fd(),
                     short(POLLIN | (channel.hasPendingOutput() ? POLLOUT : 0)),
                     0};
            ::poll(&p, 1, int(std::min<long long>(left.count(), 100)));
        }
        channel.close(1000);
        if (!answer && error.empty()) {
            error = "the daemon closed the connection";
        }
        return answer;
    }
    catch (const UnixConnectError &e) {
        error = e.what();
        return std::nullopt;
    }
}

// Who runs on a socket: the Welcome, its version, and the status (GetStatus is
// part of the frozen handshake, so every version answers it; `status` is empty
// when it did not).
struct DaemonInfo {
    mtd::ControlWelcome welcome;
    mtd::Version version;
    std::optional<mtd::Status> status;
};

inline std::optional<DaemonInfo> probeDaemon(const std::string &socketPath,
                                             std::string &error,
                                             int timeoutMs = 2000) {
    DaemonInfo info;
    bool welcomed = false;
    info.welcome.version = 0;
    const auto reply = askDaemon(
        socketPath, mtd::GetStatus{}, error, timeoutMs, &info.welcome);
    welcomed = info.welcome.version != 0;
    if (!welcomed) {
        return std::nullopt;
    }
    info.version =
        mtd::versionFromWelcome(info.welcome.build, info.welcome.version);
    if (reply) {
        if (const auto *status = std::get_if<mtd::Status>(&*reply)) {
            info.status = *status;
        }
    }
    error.clear();
    return info;
}

// A new terminal in the daemon for a client that has no window (matmux
// --new): `request` has the size, folder, command (empty: the user's shell) and
// name. Returns the session id to attach to with Hello{id}; nullopt with
// `error` set (an older daemon than 1.3.3 says it does not know the request:
// then the plain CreateSession is tried, without command and name, only when
// there was neither).
inline std::optional<std::string> createTerminal(
    const std::string &socketPath,
    const mtd::CreateTerminal &request,
    std::string &error) {
    auto reply = askDaemon(socketPath, request, error);
    // An older daemon does not know the request: it answers "bad control
    // message" (or just closes the connection).
    const auto *first =
        reply ? std::get_if<mtd::ControlError>(&*reply) : nullptr;
    const bool oldDaemon =
        (!reply && error.find("closed") != std::string::npos) ||
        (first && first->code == mtd::ControlErrorCode::BadMessage);
    if (oldDaemon) {
        if (!request.command.empty() || !request.name.empty()) {
            error =
                "this daemon is older than 1.3.3 and cannot start a program or "
                "give a name; stop it (matmux --daemon-stop) and try again";
            return std::nullopt;
        }
        error.clear();
        reply = askDaemon(
            socketPath,
            mtd::CreateSession{request.cwd, request.columns, request.rows},
            error);
    }
    if (!reply) {
        return std::nullopt;
    }
    if (const auto *created = std::get_if<mtd::SessionCreated>(&*reply)) {
        return created->id;
    }
    if (const auto *refused = std::get_if<mtd::ControlError>(&*reply)) {
        error = refused->reason;
    }
    else {
        error = "unexpected answer from the daemon";
    }
    return std::nullopt;
}

// Ends the terminal `id` in the daemon (what closing a pane does: hang up, kill
// after a short grace). False with `error` set; an older daemon (before 1.3.4)
// is named as the reason.
inline bool endSession(const std::string &socketPath,
                       const std::string &id,
                       std::string &error) {
    const auto reply = askDaemon(socketPath, mtd::EndSession{id}, error);
    if (!reply) {
        if (error.find("closed the connection") != std::string::npos ||
            error.find("unreadable") != std::string::npos) {
            error = "this daemon is older than 1.3.4 and cannot end a terminal "
                    "from the list";
        }
        return false;
    }
    if (std::holds_alternative<mtd::SessionEnded>(*reply)) {
        return true;
    }
    if (const auto *refused = std::get_if<mtd::ControlError>(&*reply)) {
        error = refused->code == mtd::ControlErrorCode::BadMessage
                    ? "this daemon is older than 1.3.4 and cannot end a "
                      "terminal from the list"
                    : refused->reason;
    }
    else {
        error = "unexpected answer from the daemon";
    }
    return false;
}

// True when something accepts connections on the socket.
inline bool daemonRunning(const std::string &socketPath) {
    try {
        UnixSocketChannel channel(socketPath);
        channel.close(1000);
        return true;
    }
    catch (const UnixConnectError &) {
        return false;
    }
}

// `program` next to this program, else its bare name (found on PATH).
inline std::string siblingExecutable(const std::string &program) {
    std::string self(4096, '\0');
    const auto n = ::readlink("/proc/self/exe", self.data(), self.size());
    if (n > 0) {
        self.resize(std::size_t(n));
        const auto slash = self.rfind('/');
        if (slash != std::string::npos) {
            const std::string beside = self.substr(0, slash + 1) + program;
            if (::access(beside.c_str(), X_OK) == 0) {
                return beside;
            }
        }
    }
    return program;
}

// matterm-daemon next to this program, else the one on PATH.
inline std::string daemonExecutable() {
    return siblingExecutable("matterm-daemon");
}

// Starts `matterm-daemon --daemonize` (with --daemon-socket when a path is
// given) and waits for it to report that the socket is ready. Safe after
// threads exist (posix_spawn). Empty on success, else what went wrong.
inline std::string startDaemon(
    const std::string &socketPath,
    const std::string &executable = daemonExecutable()) {
    std::vector<std::string> args = {executable, "--daemonize"};
    if (!socketPath.empty()) {
        args.push_back("--daemon-socket");
        args.push_back(socketPath);
    }
    std::vector<char *> argv;
    for (auto &a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(
        &pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        return "cannot start " + executable + ": " + std::strerror(rc);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return "cannot wait for the daemon to start";
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return "the daemon did not start (see its message above)";
    }
    return {};
}

// The socket to use: the one asked for, else the default.
inline std::string daemonSocket(const std::string &requested) {
    return requested.empty() ? defaultDaemonSocketPath() : requested;
}

// The daemon on `requested` (empty: the default socket), started if it is not
// there. Empty on success, else the reason.
inline std::string ensureDaemon(
    const std::string &requested,
    const std::string &executable = daemonExecutable()) {
    const auto socketPath = daemonSocket(requested);
    if (daemonRunning(socketPath)) {
        return {};
    }
    auto error = startDaemon(requested, executable);
    if (!error.empty()) {
        return error;
    }
    for (int i = 0; i < 20; ++i) {
        if (daemonRunning(socketPath)) {
            return {};
        }
        ::usleep(50000);
    }
    return "the daemon started but does not accept connections";
}

} // namespace frontend
