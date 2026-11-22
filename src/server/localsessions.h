#pragma once

// matterm-server as a client of the local daemon (docs/TABS_PLAN.md section 11, docs/PROTOCOL.md
// section 12): with --expose-local-sessions a logged-in browser may list and take over the
// terminals the owner marked shared. The daemon enforces the sharing (an "exposed" connection
// sees shared sessions only); this file is only the plumbing: one framed Unix-socket connection.
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace server {

struct LocalSessionInfo {
    std::string id;
    bool attached = false;
};

// One connection to the daemon speaking its length framing. Single io_context thread. The
// first frame the daemon sends (ControlWelcome, answering ExposedHello) is swallowed.
class DaemonConnection : public std::enable_shared_from_this<DaemonConnection> {
public:
    using OnFrame = std::function<void(std::string)>;
    using OnClosed = std::function<void()>;
    // Connects and sends ExposedHello. `onClosed` runs once, whoever ends the connection
    // (also when connecting fails).
    static std::shared_ptr<DaemonConnection> open(boost::asio::any_io_executor executor,
                                                  const std::string &socketPath,
                                                  OnFrame onFrame,
                                                  OnClosed onClosed);
    ~DaemonConnection();
    // Queues one message (the bytes a WebSocket message would carry). Dropped after close.
    void send(const std::string &payload);
    void close();
    bool isClosed() const;

private:
    struct Impl;
    void pumpRead();
    void pumpWrite();
    explicit DaemonConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Asks the daemon for the shared sessions. `done` runs once, nullopt when the daemon cannot
// be reached or answers wrongly. Bounded by `timeout`.
void listSharedSessions(boost::asio::any_io_executor executor,
                        const std::string &socketPath,
                        std::chrono::milliseconds timeout,
                        std::function<void(std::optional<std::vector<LocalSessionInfo>>)> done);

} // namespace server
