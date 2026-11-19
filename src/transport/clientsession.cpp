#include "clientsession.h"

#include <algorithm>
#include <limits>

using namespace protocol;

ClientSession::ClientSession(MessageChannel &channel, Config config)
    : channel_(channel)
    , config_(std::move(config)) {
    channel_.setEvents({[this] { onOpened(); },
                        [this](std::string_view bytes) { onMessage(bytes); },
                        [this](int code) { onClosed(code); }});
}

void ClientSession::setEvents(Events events) {
    events_ = std::move(events);
}

ClientSession::State ClientSession::state() const noexcept {
    return state_;
}
const std::string &ClientSession::sessionId() const noexcept {
    return sessionId_;
}
std::uint64_t ClientSession::receivedOffset() const noexcept {
    return received_;
}

bool ClientSession::sendMessage(const ClientMessage &message) {
    std::string bytes;
    if (!encode(message, bytes) || !channel_.send(bytes)) {
        return false;
    }
    lastSent_ = config_.clock();
    return true;
}

void ClientSession::onOpened() {
    if (state_ != State::Connecting) {
        return;
    }
    const auto now = config_.clock();
    lastReceived_ = lastSent_ = lastAck_ = now;
    Hello hello;
    hello.columns = std::uint16_t(config_.columns);
    hello.rows = std::uint16_t(config_.rows);
    hello.sessionId = config_.sessionId;
    hello.resumeOffset = config_.resumeOffset;
    hello.features = config_.replay ? featureReplay : 0;
    if (!sendMessage(hello)) {
        fail("could not send Hello", 1011);
        return;
    }
    state_ = State::AwaitWelcome;
}

void ClientSession::fail(const std::string &reason, int closeCode) {
    if (state_ == State::Failed || state_ == State::Exited) {
        return;
    }
    state_ = State::Failed;
    channel_.close(closeCode);
    if (events_.closed) {
        events_.closed(reason);
    }
}

void ClientSession::onClosed(int code) {
    if (state_ == State::Failed || state_ == State::Exited) {
        return;
    }
    fail("connection lost (close code " + std::to_string(code) + ")", code);
}

void ClientSession::onMessage(std::string_view bytes) {
    if (state_ == State::Failed || state_ == State::Exited) {
        return;
    }
    const auto decoded = decodeServer(bytes);
    if (!decoded) {
        fail(std::string("protocol error: ") + toString(decoded.error), 1002);
        return;
    }
    lastReceived_ = config_.clock();
    if (state_ == State::AwaitWelcome &&
        !std::holds_alternative<Welcome>(*decoded.message) &&
        !std::holds_alternative<Error>(*decoded.message)) {
        fail("protocol error: expected Welcome", 1002);
        return;
    }
    std::visit([&](const auto &message) { handle(message); }, *decoded.message);
}

void ClientSession::handle(const Welcome &message) {
    if (state_ != State::AwaitWelcome) {
        fail("protocol error: unexpected Welcome", 1002);
        return;
    }
    if (message.version == 0 || message.version > currentVersion) {
        fail("server chose an unsupported protocol version", 1002);
        return;
    }
    const auto offered = config_.replay ? featureReplay : 0;
    const bool resumed = (message.flags & welcomeResumed) != 0;
    const bool reset = (message.flags & welcomeOutputLost) != 0;
    if ((message.features & ~offered) || (message.flags & ~3u) ||
        (reset && !resumed) ||
        (resumed && !(message.features & featureReplay)) ||
        (resumed ? (config_.sessionId.empty() ||
                    message.sessionId != config_.sessionId)
                 : (!config_.sessionId.empty() || message.startOffset != 0 ||
                    message.flags != 0)) ||
        (resumed && !reset && message.startOffset != config_.resumeOffset)) {
        fail("protocol error: invalid replay negotiation", 1002);
        return;
    }
    if (reset) {
        if (!config_.resetForReplay) {
            fail("replay requires a frontend reset callback", 1002);
            return;
        }
        try {
            config_.resetForReplay();
        }
        catch (const std::exception &) {
            fail("frontend replay reset failed", 1011);
            return;
        }
    }
    sessionId_ = message.sessionId;
    negotiatedFeatures_ = message.features;
    received_ = acked_ = message.startOffset;
    lastAck_ = config_.clock();
    state_ = State::Running;
}

void ClientSession::handle(const Output &message) {
    if (state_ != State::Running) {
        fail("protocol error: Output before Welcome", 1002);
        return;
    }
    if (message.offset != received_) {
        fail("protocol error: gap in the output stream", 1002);
        return;
    }
    if (message.data.size() >
        std::numeric_limits<std::uint64_t>::max() - received_) {
        fail("protocol error: output offset exhausted", 1002);
        return;
    }
    received_ += message.data.size();
    if (events_.output) {
        events_.output(message.data); // Parsed by the time this returns.
    }
    if (state_ == State::Running &&
        received_ - acked_ >= config_.ackThreshold) {
        sendAck();
    }
}

void ClientSession::handle(const Exit &message) {
    if (state_ != State::Running) {
        fail("protocol error: unexpected Exit", 1002);
        return;
    }
    if (received_ != acked_) {
        sendAck(); // Let the server see everything was consumed.
    }
    state_ = State::Exited;
    if (events_.exited) {
        events_.exited(message.code, message.signal);
    }
}

void ClientSession::handle(const Pong &message) {
    if (pingSentAt_ && message.nonce == pingNonce_) {
        pingSentAt_.reset();
    }
}

void ClientSession::handle(const protocol::Error &message) {
    fail(std::string("server error ") + toString(message.code) + ": " +
             message.reason,
         1000);
}

void ClientSession::sendAck() {
    if (sendMessage(Ack{received_})) {
        acked_ = received_;
        lastAck_ = config_.clock();
    }
}

bool ClientSession::sendInput(std::string_view bytes) {
    if (state_ != State::AwaitWelcome && state_ != State::Running) {
        return false;
    }
    while (!bytes.empty()) {
        const auto size = std::min(bytes.size(), config_.maxInputChunk);
        if (!sendMessage(Input{std::string(bytes.substr(0, size))})) {
            return false;
        }
        bytes.remove_prefix(size);
    }
    return true;
}

void ClientSession::resize(int columns,
                           int rows,
                           int pixelWidth,
                           int pixelHeight) {
    config_.columns = columns;
    config_.rows = rows;
    if (state_ != State::AwaitWelcome && state_ != State::Running) {
        return; // The size is sent in Hello when the channel opens.
    }
    const auto clamp = [](int value, int low, int high) {
        return std::uint16_t(std::clamp(value, low, high));
    };
    sendMessage(Resize{clamp(columns, 1, maxDimension),
                       clamp(rows, 1, maxDimension),
                       clamp(pixelWidth, 0, 65535),
                       clamp(pixelHeight, 0, 65535)});
}

void ClientSession::close() {
    if (state_ == State::AwaitWelcome || state_ == State::Running) {
        sendMessage(Close{});
    }
}

std::optional<Time> ClientSession::nextDeadline() const {
    if (state_ != State::Running && state_ != State::AwaitWelcome) {
        return std::nullopt;
    }
    std::optional<Time> result;
    const auto consider = [&](Time time) {
        if (!result || time < *result) {
            result = time;
        }
    };
    if (received_ != acked_) {
        consider(lastAck_ + config_.ackInterval);
    }
    if (pingSentAt_) {
        consider(std::max(*pingSentAt_, lastReceived_) + config_.pongTimeout);
    }
    else {
        consider(lastSent_ + config_.pingInterval);
    }
    return result;
}

void ClientSession::tick() {
    if (state_ != State::Running && state_ != State::AwaitWelcome) {
        return;
    }
    const auto now = config_.clock();
    if (received_ != acked_ && now - lastAck_ >= config_.ackInterval) {
        sendAck();
    }
    if (pingSentAt_) {
        if (now - std::max(*pingSentAt_, lastReceived_) >=
            config_.pongTimeout) {
            fail("server is not responding", 1001);
        }
    }
    else if (now - lastSent_ >= config_.pingInterval) {
        pingSentAt_ = now;
        sendMessage(Ping{++pingNonce_});
    }
}
