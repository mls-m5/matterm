#pragma once

#include "message.h"
#include "protocoltime.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace protocol {

struct SessionConfig {
    using ms = std::chrono::milliseconds;
    std::string sessionId = "session"; // Reported in Welcome; chosen by host.
    ms helloTimeout{5000};
    ms idleTimeout{60000};
    ms ackStallTimeout{30000};
    std::size_t outputWindow = 262144;
    ms resizeInterval{100};         // At most 10 applied resizes per second.
    unsigned resizeHardLimit = 100; // Per second; more is RateLimited.
    ms rateWindow{5000};
    unsigned maxMessagesPerWindow = 5000; // 1000 per second on average.
};

// Everything the session needs from its environment. The server implements it
// with a real transport and PTY; tests implement it with fakes.
class SessionHost {
public:
    virtual ~SessionHost() = default;
    // One complete encoded server message for the transport.
    virtual void send(std::string bytes) = 0;
    // WebSocket-style close code (1000 normal, 1002, 1008, 1009, 1011, ...).
    virtual void closeTransport(int code) = 0;
    virtual bool startProcess(std::uint16_t columns, std::uint16_t rows) = 0;
    // False when the host's pending-input queue limit would be exceeded.
    virtual bool writeInput(std::string_view bytes) = 0;
    virtual void resizeProcess(std::uint16_t columns,
                               std::uint16_t rows,
                               std::uint16_t pixelWidth,
                               std::uint16_t pixelHeight) = 0;
    // SIGHUP to the process group, then SIGKILL later. The host reports the
    // end through ServerSession::onProcessExit.
    virtual void terminateProcess() = 0;
};

// Server side of one connection (protocol version 1). Transport independent:
// feed it complete messages and process events, and it calls the host.
class ServerSession {
public:
    enum class State { Idle, AwaitHello, Running, Closing, Closed };

    ServerSession(SessionConfig config, SessionHost &host);

    // The transport was accepted (after authentication): wait for Hello.
    void start(Time now);
    // One complete transport message from the client.
    void onMessage(std::string_view bytes, Time now);
    // The transport went away without a protocol close.
    void onTransportClosed(Time now);
    // PTY output. The host should read at most outputBudget() bytes at a time;
    // larger input is still sent, but exceeds the window.
    void onProcessOutput(std::string_view bytes, Time now);
    // The process ended and all its output has been passed to
    // onProcessOutput. `code` is -1 when killed by a signal.
    void onProcessExit(int code, int signal, Time now);
    // Timeouts and coalesced resizes. Call at least at nextDeadline().
    void tick(Time now);
    // The server is stopping.
    void shutdown(Time now);

    State state() const noexcept;
    // Bytes the host may read from the PTY now: window minus unacknowledged.
    std::size_t outputBudget() const noexcept;
    // Earliest time at which tick() has work, if any.
    std::optional<Time> nextDeadline() const noexcept;
    std::uint64_t sentOffset() const noexcept;
    std::uint64_t ackedOffset() const noexcept;

private:
    void fail(ErrorCode code, const char *reason);
    void sendMessage(const ServerMessage &message);
    void handle(const Hello &message, Time now);
    void handle(const Input &message, Time now);
    void handle(const Resize &message, Time now);
    void handle(const Ack &message, Time now);
    void handle(const Ping &message, Time now);
    void handle(const Close &message, Time now);
    void applyResize(Time now);
    void updateStall(Time now);

    SessionConfig config_;
    SessionHost &host_;
    State state_ = State::Idle;
    bool processStarted_ = false;
    bool processExited_ = false;

    Time helloDeadline_{};
    Time lastMessage_{};
    std::uint64_t sent_ = 0;
    std::uint64_t acked_ = 0;
    std::optional<Time> stallSince_;

    std::optional<Resize> pendingResize_;
    Time lastResizeApplied_{};
    Time resizeSecondStart_{};
    unsigned resizesInSecond_ = 0;

    Time rateWindowStart_{};
    unsigned messagesInWindow_ = 0;
};

// WebSocket close code that accompanies an Error message.
int closeCodeFor(ErrorCode code) noexcept;

} // namespace protocol
