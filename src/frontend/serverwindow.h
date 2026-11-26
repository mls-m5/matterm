#pragma once

// Start a separate GUI process for a saved SSH destination. This never
// connects, changes panes, or restarts a daemon in the current window.
#include "sshhost.h"

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

namespace frontend {
inline bool spawnServerWindow(const std::string &executable,
                              const std::string &host,
                              pid_t &child,
                              std::string &error) {
    if (!safeSshHost(host)) {
        error = "invalid SSH host";
        return false;
    }
    const char *const args[] = {
        executable.c_str(), "--daemon-host", host.c_str(), nullptr};
    posix_spawnattr_t attributes;
    const int init = posix_spawnattr_init(&attributes);
    if (init != 0) {
        error =
            "cannot prepare new window: " + std::string(std::strerror(init));
        return false;
    }
    // Do not send the terminal's Ctrl+C to both GUI processes.
    int rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (rc == 0)
        rc = posix_spawnattr_setpgroup(&attributes, 0);
    if (rc == 0) {
        rc = posix_spawn(&child,
                         executable.c_str(),
                         nullptr,
                         &attributes,
                         const_cast<char *const *>(args),
                         environ);
    }
    posix_spawnattr_destroy(&attributes);
    if (rc != 0) {
        error = "cannot open new window: " + std::string(std::strerror(rc));
        return false;
    }
    return true;
}
} // namespace frontend
