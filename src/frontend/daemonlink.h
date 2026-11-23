#pragma once

// A native pane that lives in the local daemon (docs/tasks/daemon.md,
// docs/TABS_PLAN.md section 10): a TerminalLink over a ClientSession over a
// message channel, normally the Unix socket of matterm-daemon. It is the same
// client path as the browser's. The terminal's process belongs to the daemon,
// so a GUI that goes away (restart, crash) leaves it running, and a new GUI
// attaches to the same session id.
//
// Ownership: the pane owns the link and the TerminalView, and declares the link
// first. Before the link is destroyed or replaced the view must let go of it
// (`view.detach()`), as attachToDaemon() does for the one it replaces.
//
// close() ends the terminal in the daemon (a protocol Close). To leave it
// running, call detach() or just destroy the link: the connection drops, the
// session stays.
#include "clientsession.h"
#include "terminalview.h"
#include "unixchannel.h"

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace frontend {

class DaemonLink final : public TerminalLink {
public:
    // Called by tick() before the session runs: a channel that needs polling
    // (the Unix socket) is polled here; a channel that delivers by itself needs
    // nothing.
    using Pump = std::function<void()>;

    DaemonLink(std::unique_ptr<MessageChannel> channel,
               ClientSession::Config config,
               Pump pump = {})
        : channel_(std::move(channel))
        , requestedId_(config.sessionId)
        , pump_(std::move(pump))
        , session_(*channel_, std::move(config)) {}

    // Connects to the daemon's socket; throws UnixConnectError ("no daemon
    // there" is ENOENT or ECONNREFUSED).
    static std::unique_ptr<DaemonLink> connect(const std::string &socketPath,
                                               ClientSession::Config config) {
        auto channel = std::make_unique<UnixSocketChannel>(socketPath);
        auto *raw = channel.get();
        auto link = std::make_unique<DaemonLink>(
            std::move(channel), std::move(config), [raw] { raw->poll(); });
        link->socket_ = raw;
        return link;
    }

    // For a loop that sleeps in poll(2) instead of ticking every frame: the
    // descriptor to wait on (-1: none, or not a Unix socket) and whether it
    // also has to be watched for writing.
    int pollFd() const {
        return socket_ ? socket_->fd() : -1;
    }
    bool wantsWrite() const {
        return socket_ && socket_->hasPendingOutput();
    }

    // Drives the connection and the session (acks, pings). Call it every frame.
    void tick() {
        if (pump_) {
            pump_();
        }
        session_.tick();
    }
    std::optional<protocol::Time> nextDeadline() const {
        return session_.nextDeadline();
    }

    // TerminalLink.
    void setEvents(Events events) override {
        session_.setEvents(std::move(events));
    }
    bool sendInput(std::string_view bytes) override {
        return session_.sendInput(bytes);
    }
    void resize(int columns,
                int rows,
                int pixelWidth = 0,
                int pixelHeight = 0) override {
        session_.resize(columns, rows, pixelWidth, pixelHeight);
    }
    void close() override {
        session_.close(); // The terminal ends in the daemon.
        if (pump_) {
            pump_(); // Let the message go out now; the owner may drop the link
                     // next.
        }
    }
    // Leaves the terminal running in the daemon and ends only this connection.
    void detach() {
        channel_->close(1000);
        if (pump_) {
            pump_();
        }
    }

    ClientSession &session() {
        return session_;
    }
    const ClientSession &session() const {
        return session_;
    }
    // The id the daemon keeps the terminal under (empty until its Welcome).
    const std::string &sessionId() const {
        return session_.sessionId();
    }
    // The id this link was asked to attach to (empty: a new terminal).
    const std::string &requestedId() const {
        return requestedId_;
    }
    // The id to attach to again: the confirmed one, else the one that was asked
    // for.
    std::string knownId() const {
        return sessionId().empty() ? requestedId_ : sessionId();
    }
    std::uint64_t receivedOffset() const {
        return session_.receivedOffset();
    }
    std::optional<protocol::ErrorCode> errorCode() const {
        return session_.errorCode();
    }

private:
    std::unique_ptr<MessageChannel> channel_;
    std::string requestedId_;
    const UnixSocketChannel *socket_ =
        nullptr; // Owned by channel_, when it is one.
    Pump pump_;
    ClientSession session_; // Last: it listens to the channel above.
};

// What Enter does in a pane whose terminal is not live any more (the pane says
// so in its text). Nothing takes a terminal back by itself: this is the user's
// decision.
enum class Reattach {
    None,  // Live (nothing to do) or the program ended (the owner closes the
           // pane).
    Same,  // Lost connection or taken over: attach again, from the replay
           // offset.
    Fresh, // The daemon does not know the session any more: a new terminal.
};

inline Reattach reattachKind(const TerminalView &view) {
    switch (view.state()) {
    case TerminalView::State::Disconnected:
    case TerminalView::State::Replaced:
        return Reattach::Same;
    case TerminalView::State::Unavailable:
        return Reattach::Fresh;
    case TerminalView::State::Live:
    case TerminalView::State::Exited:
        break;
    }
    return Reattach::None;
}

// Connects `view` to the daemon at `socketPath`: a new terminal when
// `sessionId` is empty, else that session, continuing after `resumeOffset`
// bytes (the view's parsed state is kept; 0 asks for everything the daemon
// still holds). The view is first let go of whatever it listened to. On failure
// (no daemon) the view says so and gets a disconnected state that may be
// retried; the result is null.
inline std::unique_ptr<DaemonLink> attachToDaemon(
    TerminalView &view,
    const std::string &socketPath,
    std::string sessionId,
    std::uint64_t resumeOffset,
    std::function<protocol::Time()> clock) {
    view.detach();
    ClientSession::Config config;
    config.clock = std::move(clock);
    config.extraFeatures = protocol::featureDaemon;
    view.configure(config, std::move(sessionId), resumeOffset);
    try {
        auto link = DaemonLink::connect(socketPath, std::move(config));
        auto *raw = link.get();
        view.attach(*link, [raw] { return raw->errorCode(); });
        return link;
    }
    catch (const UnixConnectError &error) {
        const bool missing =
            error.error() == ENOENT || error.error() == ECONNREFUSED;
        view.linkFailed(missing ? "the daemon is not running"
                                : "cannot reach the daemon");
        return nullptr;
    }
}

// Enter in a dead pane: attaches again as reattachKind() says, from what the
// old link knew. Returns the new link (null when nothing was done or the daemon
// is not there).
inline std::unique_ptr<DaemonLink> reattachToDaemon(
    TerminalView &view,
    const DaemonLink *old,
    const std::string &socketPath,
    std::function<protocol::Time()> clock) {
    const auto kind = reattachKind(view);
    if (kind == Reattach::None) {
        return nullptr;
    }
    std::string id = old ? old->knownId() : std::string();
    const std::uint64_t offset = old && !id.empty() ? old->receivedOffset() : 0;
    if (kind == Reattach::Fresh) {
        id.clear();
    }
    // Before `id` is moved: the order of evaluation of the arguments is not
    // specified.
    const std::uint64_t resumeOffset = id.empty() ? 0 : offset;
    return attachToDaemon(
        view, socketPath, std::move(id), resumeOffset, std::move(clock));
}

} // namespace frontend
