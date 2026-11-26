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

inline bool sendServerStatus(int fd,
                             char kind,
                             const std::string &message = {}) {
    if (fd < 0)
        return false;
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
    return ::send(fd, line.data(), line.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(line.size());
}

// Only a visible decision in the original GUI may authorize a remote copy.
// If the GUI closes while waiting, recv returns EOF and installation is denied.
inline bool requestServerConfirmation(int fd, const std::string &question) {
    if (fd < 0)
        return false;
    if (!sendServerStatus(fd, 'I', question))
        return false;
    char answer = 0;
    ssize_t count;
    do {
        count = ::recv(fd, &answer, 1, 0);
    } while (count < 0 && errno == EINTR);
    return count == 1 && answer == 'y';
}

inline bool answerServerConfirmation(int fd, bool yes) {
    const char answer = yes ? 'y' : 'n';
    return fd >= 0 && ::send(fd, &answer, 1, MSG_NOSIGNAL) == 1;
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
