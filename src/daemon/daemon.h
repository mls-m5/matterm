#pragma once

// The local daemon core (docs/tasks/daemon.md workstream A): owns PTYs and their replay rings in
// a SessionRegistry and serves them on a Unix socket with the length framing of framing.h.
// No UI concepts, no network, no password: the socket is private to the user (SO_PEERCRED).
#include "ptysessionhost.h"
#include "sessionregistry.h"
#include "socketpath.h"
#include "version.h"

#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace mtd {

struct DaemonConfig {
    std::string socketPath;
    std::string perfLogPath; // Empty uses MATTERM_PERF_LOG, otherwise explicit
                             // --perf-log.
    // True for the default location: the parent directory must be ours and 0700 (created if
    // missing). A --daemon-socket path may live anywhere, its directory is left alone.
    bool privateDirectory = true;
    std::optional<uid_t> allowedUid;     // Default: the daemon's own effective uid. For tests.
    unsigned idleExitSeconds = 60;       // No sessions and no client for this long: exit. 0 = never.
    unsigned maxClients = 32;
    unsigned helloTimeoutMs = 5000;
    server::ProcessConfig process;
    server::RegistryConfig registry;     // Filled with daemon defaults by defaultConfig().
    std::string build =
        "matterm-daemon"; // The program's name in the Welcome text.
    // The control version it announces and enforces. Tests change it to fake
    // another daemon.
    Version version = ownVersion();
};

// Persistent sessions, large rings, detached sessions never expire, offers the "daemon" feature.
DaemonConfig defaultConfig();

enum class ExitReason { None, Idle, Shutdown, Stopped };

class Daemon {
public:
    explicit Daemon(DaemonConfig config);
    ~Daemon();
    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    // Takes the lock file, replaces a stale socket, binds and listens. False with a reason when
    // another daemon owns the socket or anything fails.
    bool start(std::string &error);
    // Serves until idle exit, a confirmed Shutdown or stop(). Ends all sessions on the way out.
    void run();
    // Thread-safe.
    void stop();
    ExitReason reason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mtd
