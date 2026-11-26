#pragma once

// Launch a separate GUI process. The socket reports progress and an explicit
// first-frame acknowledgement to the temporary tab in the original window.
#include "sshhost.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

extern char **environ;

namespace frontend {
struct ServerWindow {
    pid_t pid = -1;
    int statusFd = -1;
};

inline void sendServerStatus(int fd,
                             char kind,
                             const std::string &message = {}) {
    if (fd < 0)
        return;
    std::string line(1, kind);
    if (!message.empty())
        line += ' ';
    for (unsigned char c : message.substr(0, 2048)) {
        if (c == '\n' || c == '\r')
            line += ' ';
        else if (c >= 32 && c != 127)
            line += char(c);
    }
    line += '\n';
    ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
}

inline bool spawnServerWindow(const std::string &executable,
                              const std::string &host,
                              ServerWindow &window,
                              std::string &error) {
    if (!safeSshHost(host)) {
        error = "invalid SSH host";
        return false;
    }
    int status[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, status) != 0) {
        error = "cannot prepare startup feedback: " +
                std::string(std::strerror(errno));
        return false;
    }
    // dup2(fd, fd) would retain CLOEXEC, so fd 3 must be a distinct target.
    if (status[1] == 3) {
        const int copy = ::fcntl(status[1], F_DUPFD_CLOEXEC, 4);
        if (copy < 0) {
            error = "cannot prepare startup feedback: " +
                    std::string(std::strerror(errno));
            ::close(status[0]);
            ::close(status[1]);
            return false;
        }
        ::close(status[1]);
        status[1] = copy;
    }
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t actions;
    int rc = posix_spawnattr_init(&attributes);
    if (rc == 0) {
        rc = posix_spawn_file_actions_init(&actions);
        if (rc != 0)
            posix_spawnattr_destroy(&attributes);
    }
    if (rc == 0) {
        // The read end can itself be fd 3: close it before dup2 in the child.
        rc = posix_spawn_file_actions_addclose(&actions, status[0]);
        if (rc == 0)
            rc = posix_spawn_file_actions_adddup2(&actions, status[1], 3);
        if (rc == 0 && status[1] != 3)
            rc = posix_spawn_file_actions_addclose(&actions, status[1]);
        // Never block on an invisible terminal prompt in this GUI workflow.
        if (rc == 0)
            rc = posix_spawn_file_actions_addopen(
                &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (rc == 0)
            rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (rc == 0)
            rc = posix_spawnattr_setpgroup(&attributes, 0);
        if (rc == 0) {
            const char *const args[] = {executable.c_str(),
                                        "--daemon-host",
                                        host.c_str(),
                                        "--server-ready-fd",
                                        "3",
                                        nullptr};
            rc = posix_spawn(&window.pid,
                             executable.c_str(),
                             &actions,
                             &attributes,
                             const_cast<char *const *>(args),
                             environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    }
    ::close(status[1]);
    if (rc != 0) {
        ::close(status[0]);
        error = "cannot open new window: " + std::string(std::strerror(rc));
        return false;
    }
    window.statusFd = status[0];
    ::fcntl(window.statusFd,
            F_SETFL,
            ::fcntl(window.statusFd, F_GETFL) | O_NONBLOCK);
    return true;
}
} // namespace frontend
