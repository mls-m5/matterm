#pragma once

#include "message.h"
#include "messagechannel.h"
#include "protocoltime.h"
#include "terminallink.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// Client side of one matterm-tty connection (protocol version 1; see
// docs/PROTOCOL.md). A TerminalLink over a MessageChannel: sends Hello when the
// channel opens, chunks input, acknowledges parsed output, keeps the
// connection alive with Ping, and reports exit or failure. Call tick() from the
// front end's frame or timer loop, at least by nextDeadline().
class ClientSession final : public TerminalLink {
public:
    using ms = std::chrono::milliseconds;
    struct Config {
        std::function<protocol::Time()> clock; // Required.
        int columns = 80;
        int rows = 24;
        std::size_t ackThreshold = 16384; // Ack at once past this much output.
        ms ackInterval{100};              // Otherwise ack at most this late.
        ms pingInterval{20000};           // Idle time before a Ping.
        ms pongTimeout{30000};            // Silence after a Ping is fatal.
        std::size_t maxInputChunk = 4096;
        bool replay = false;
        // Further feature bits to offer in Hello (featureDaemon for the local
        // daemon).
        std::uint32_t extraFeatures = 0;
        std::string sessionId;
        std::uint64_t resumeOffset = 0;
        // Required before accepting a reset/full replay (Welcome.flags=3).
        // Reset the frontend parser/model/selection before any Output callback.
        std::function<void()> resetForReplay;
    };
    enum class State { Connecting, AwaitWelcome, Running, Exited, Failed };

    // Takes over the channel's events. The channel must outlive the session.
    ClientSession(MessageChannel &channel, Config config);

    void setEvents(Events events) override;
    bool sendInput(std::string_view bytes) override;
    void resize(int columns,
                int rows,
                int pixelWidth = 0,
                int pixelHeight = 0) override;
    void close() override;

    void tick();
    // Measures the round trip to the peer: sends a Ping of its own (the
    // keep-alive is not touched) unless one is out, and times its Pong.
    // takeRoundTripMs() gives the last measurement once (nullopt when none
    // arrived).
    void probeRoundTrip();
    // Encoded protocol payload bytes per accepted/sent message. A Unix socket
    // caller adds its four-byte framing prefix; no socket chunk counting.
    void setTrafficObserver(std::function<void(bool, std::size_t)> observer) {
        trafficObserver_ = std::move(observer);
    }
    std::optional<double> takeRoundTripMs() {
        auto r = roundTripMs_;
        roundTripMs_.reset();
        return r;
    }
    std::optional<protocol::Time> nextDeadline() const;
    State state() const noexcept;
    const std::string &sessionId() const noexcept;
    std::uint64_t receivedOffset() const noexcept;
    std::uint32_t negotiatedFeatures() const noexcept {
        return negotiatedFeatures_;
    }
    // Empty for local validation, transport failures and normal Exit.
    std::optional<protocol::ErrorCode> errorCode() const noexcept {
        return errorCode_;
    }
    // The WebSocket close code the peer or the network ended the transport with
    // (1000 normal, 1006 abnormal). Empty while open, after a normal Exit, and
    // when this side closed the channel for a protocol error.
    std::optional<int> closeCode() const noexcept {
        return closeCode_;
    }

private:
    void onOpened();
    void onMessage(std::string_view bytes);
    void onClosed(int code);
    void handle(const protocol::Welcome &message);
    void handle(const protocol::Output &message);
    void handle(const protocol::Exit &message);
    void handle(const protocol::Pong &message);
    void handle(const protocol::Error &message);
    bool sendMessage(const protocol::ClientMessage &message);
    void sendAck();
    void fail(const std::string &reason, int closeCode);

    MessageChannel &channel_;
    Config config_;
    Events events_;
    State state_ = State::Connecting;
    std::string sessionId_;
    std::uint32_t negotiatedFeatures_ = 0;
    std::optional<protocol::ErrorCode> errorCode_;
    std::optional<int> closeCode_;
    std::uint64_t received_ = 0;
    std::uint64_t acked_ = 0;
    protocol::Time lastAck_{};
    protocol::Time lastSent_{};
    protocol::Time lastReceived_{};
    std::optional<protocol::Time> pingSentAt_;
    std::optional<protocol::Time> probeSentAt_;
    std::uint32_t probeNonce_ = 0;
    std::function<void(bool, std::size_t)> trafficObserver_;
    std::optional<double> roundTripMs_;
    std::uint32_t pingNonce_ = 0;
    std::uint32_t keepaliveNonce_ = 0;
};
