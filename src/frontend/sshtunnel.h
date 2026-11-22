#pragma once

// A matterm-daemon on another computer, reached through OpenSSH's Unix-socket forwarding
// (docs/REMOTE_DAEMON.md): the daemon's socket there is forwarded to a private socket here, and the
// native app uses that socket exactly like its own daemon's. Nothing of matterm runs on the wire:
// ssh carries the daemon's framing (docs/PROTOCOL.md "Local daemon") as it is.
//
//   1. `ssh host matterm-daemon --ensure --print-socket`: starts the daemon there if it is not
//      running and says where its socket is (the path depends on the remote $XDG_RUNTIME_DIR).
//   2. `ssh -N -L LOCAL:REMOTE host` in the background, LOCAL inside a 0700 directory of ours.
//   3. Wait until LOCAL accepts connections (that is, the remote daemon answered through the
//      tunnel), watching the ssh process meanwhile.
//
// Who is who: the daemon checks SO_PEERCRED of what connects to it. Through the tunnel that is
// sshd's child running as the account you logged in as, so the daemon accepts exactly the
// account it belongs to. Our side checks the same of the local socket (UnixSocketChannel): the
// peer is our own ssh process; the 0700 directory keeps other local users away from it.
//
// Blocking calls, for startup; the tunnel object lives as long as the window does.
#include "daemonclient.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace frontend {

struct SshTunnelOptions {
    std::string host;                    // [user@]host as ssh reads it (ssh_config applies).
    std::vector<std::string> sshArguments; // Extra ssh arguments, before ours (ssh keeps the first value of an option).
    std::string ssh = "ssh";             // The ssh program.
    std::string remoteDaemon = "matterm-daemon"; // How the remote shell finds the daemon.
    std::string remoteSocket;            // Empty: the remote default ($XDG_RUNTIME_DIR/matterm/daemon.sock).
    int timeoutMs = 20000;               // For the whole startup.
};

// What may be handed to the remote shell or to ssh as a name: no spaces, quotes, `$`, `;` and so on,
// and not starting with '-' (it would be an option).
inline bool safeSshWord(const std::string &s, bool allowSlash) {
    if (s.empty() || s.size() > 1024 || s.front() == '-') {
        return false;
    }
    for (const char c : s) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == ':' || c == '%' ||
                           c == '[' || c == ']' || c == '=' || c == ',' || (allowSlash && c == '/');
        if (!plain) {
            return false;
        }
    }
    return true;
}

class SshTunnel {
public:
    // Null with `error` set when the tunnel could not be made.
    static std::unique_ptr<SshTunnel> open(const SshTunnelOptions &options, std::string &error) {
        if (!safeSshWord(options.host, false)) {
            error = "--daemon-host: not a host name ssh can be given safely";
            return nullptr;
        }
        if (!safeSshWord(options.remoteDaemon, true) ||
            (!options.remoteSocket.empty() && (!safeSshWord(options.remoteSocket, true) || options.remoteSocket.front() != '/'))) {
            error = "--daemon-host: the remote daemon or socket name has characters the remote shell would read";
            return nullptr;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
        auto tunnel = std::unique_ptr<SshTunnel>(new SshTunnel());
        // 1. Where is the socket, and is the daemon there?
        std::vector<std::string> ask = baseArguments(options);
        ask.push_back("--");
        ask.push_back(options.host);
        ask.push_back(options.remoteDaemon);
        ask.push_back("--ensure");
        ask.push_back("--print-socket");
        if (!options.remoteSocket.empty()) {
            ask.push_back("--daemon-socket");
            ask.push_back(options.remoteSocket);
        }
        std::string output;
        if (!runCapture(options.ssh, ask, deadline, output, error)) {
            error = "ssh " + options.host + ": " + error;
            return nullptr;
        }
        const auto remote = lastLine(output);
        if (remote.empty() || remote.front() != '/' || !safeSshWord(remote, true)) {
            error = "ssh " + options.host + ": the remote matterm-daemon did not say where its socket is (is "
                    "matterm-daemon installed there and on PATH?)";
            return nullptr;
        }
        // 2. The private place for our end.
        if (!tunnel->makeDirectory(error)) {
            return nullptr;
        }
        tunnel->remote_ = remote;
        std::vector<std::string> forward = baseArguments(options);
        forward.insert(forward.end(), {"-N", "-o", "ExitOnForwardFailure=yes", "-o", "StreamLocalBindUnlink=yes",
                                       "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", "-L",
                                       tunnel->socket_ + ":" + remote, "--", options.host});
        if (!tunnel->spawn(options.ssh, forward, error)) {
            return nullptr;
        }
        // 3. Through the tunnel to the daemon.
        while (true) {
            if (daemonRunning(tunnel->socket_)) {
                return tunnel;
            }
            if (!tunnel->alive()) {
                error = "ssh " + options.host + ": the tunnel ended" + tunnel->diagnostics();
                return nullptr;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "ssh " + options.host + ": the remote daemon did not answer through the tunnel" +
                        tunnel->diagnostics();
                return nullptr;
            }
            ::usleep(50000);
        }
    }

    ~SshTunnel() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            for (int i = 0; i < 100 && ::waitpid(pid_, &status, WNOHANG) == 0; ++i) {
                ::usleep(20000);
            }
            if (::waitpid(pid_, &status, WNOHANG) == 0) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
            }
        }
        if (!socket_.empty()) {
            ::unlink(socket_.c_str());
        }
        if (!errorFile_.empty()) {
            ::unlink(errorFile_.c_str());
        }
        if (!directory_.empty()) {
            ::rmdir(directory_.c_str());
        }
    }
    SshTunnel(const SshTunnel &) = delete;
    SshTunnel &operator=(const SshTunnel &) = delete;

    // The local socket to use as the daemon socket.
    const std::string &socketPath() const noexcept {
        return socket_;
    }
    // The socket's path on the other computer.
    const std::string &remoteSocketPath() const noexcept {
        return remote_;
    }
    // The ssh process (diagnostics, tests).
    pid_t processId() const noexcept {
        return pid_;
    }
    // False once the ssh process has ended (the connection to the other computer is gone).
    bool alive() {
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == 0) {
            return true;
        }
        pid_ = -1;
        return false;
    }

private:
    SshTunnel() = default;

    static std::vector<std::string> baseArguments(const SshTunnelOptions &o) {
        std::vector<std::string> a = o.sshArguments;
        // No questions: this runs without a terminal as often as with one. Given first, a user's own
        // -o BatchMode=no wins (ssh keeps the first value of an option).
        a.insert(a.end(), {"-o", "BatchMode=yes", "-o", "ConnectTimeout=10"});
        return a;
    }
    static std::string lastLine(const std::string &text) {
        auto end = text.find_last_not_of(" \r\n\t");
        if (end == std::string::npos) {
            return {};
        }
        const auto begin = text.find_last_of('\n', end);
        return text.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1) + 1);
    }

    static std::vector<char *> pointers(std::vector<std::string> &strings) {
        std::vector<char *> argv;
        for (auto &s : strings) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        return argv;
    }

    // Runs `program arguments`, collects stdout (stderr is kept for the error text).
    static bool runCapture(const std::string &program,
                           std::vector<std::string> arguments,
                           std::chrono::steady_clock::time_point deadline,
                           std::string &out,
                           std::string &error) {
        arguments.insert(arguments.begin(), program);
        int outPipe[2], errPipe[2];
        if (::pipe(outPipe) != 0) {
            error = "pipe failed";
            return false;
        }
        if (::pipe(errPipe) != 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            error = "pipe failed";
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        for (const int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            posix_spawn_file_actions_addclose(&actions, fd);
        }
        auto argv = pointers(arguments);
        pid_t pid = 0;
        const int rc = ::posix_spawnp(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        if (rc != 0) {
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            error = "cannot start " + program + ": " + std::strerror(rc);
            return false;
        }
        std::string err;
        bool outOpen = true, errOpen = true;
        bool timedOut = false;
        while (outOpen || errOpen) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                timedOut = true;
                break;
            }
            pollfd fds[2] = {{outPipe[0], short(outOpen ? POLLIN : 0), 0}, {errPipe[0], short(errOpen ? POLLIN : 0), 0}};
            if (::poll(fds, 2, int(std::min<long long>(left.count(), 200))) < 0 && errno != EINTR) {
                break;
            }
            for (int i = 0; i < 2; ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                    continue;
                }
                char buffer[1024];
                const auto n = ::read(fds[i].fd, buffer, sizeof buffer);
                std::string &target = i == 0 ? out : err;
                if (n > 0) {
                    if (target.size() < 65536) {
                        target.append(buffer, std::size_t(n));
                    }
                }
                else if (n == 0 || errno != EINTR) {
                    (i == 0 ? outOpen : errOpen) = false;
                }
            }
        }
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        if (timedOut) {
            ::kill(pid, SIGKILL);
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (timedOut) {
            error = "no answer in time";
            return false;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) {
                err.pop_back();
            }
            error = err.empty() ? "failed (exit status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")" : err;
            return false;
        }
        return true;
    }

    bool makeDirectory(std::string &error) {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        std::string base = runtime && *runtime == '/' ? std::string(runtime) : std::string("/tmp");
        std::string pattern = base + "/matterm-ssh-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (!::mkdtemp(buffer.data())) {
            error = "cannot make a private directory in " + base + ": " + std::strerror(errno);
            return false;
        }
        directory_ = buffer.data(); // mkdtemp: owner only (0700).
        socket_ = directory_ + "/d.sock";
        errorFile_ = directory_ + "/ssh.err";
        if (socket_.size() >= sizeof(sockaddr_un::sun_path)) {
            error = "the path of the local socket is too long (" + socket_ + "); set XDG_RUNTIME_DIR to something shorter";
            return false;
        }
        return true;
    }

    bool spawn(const std::string &program, std::vector<std::string> arguments, std::string &error) {
        arguments.insert(arguments.begin(), program);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 2, errorFile_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        auto argv = pointers(arguments);
        const int rc = ::posix_spawnp(&pid_, program.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) {
            pid_ = -1;
            error = "cannot start " + program + ": " + std::strerror(rc);
            return false;
        }
        return true;
    }

    std::string diagnostics() const {
        std::string text;
        if (const int fd = ::open(errorFile_.c_str(), O_RDONLY); fd >= 0) {
            char buffer[1024];
            const auto n = ::read(fd, buffer, sizeof buffer - 1);
            ::close(fd);
            if (n > 0) {
                text.assign(buffer, std::size_t(n));
            }
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        return text.empty() ? std::string() : ": " + text;
    }

    pid_t pid_ = -1;
    std::string directory_, socket_, errorFile_, remote_;
};

} // namespace frontend
