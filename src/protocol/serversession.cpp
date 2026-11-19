#include "serversession.h"

#include <algorithm>

namespace protocol {

int closeCodeFor(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::BadMessage:
    case ErrorCode::UnsupportedVersion:
        return 1002;
    case ErrorCode::LimitExceeded:
        return 1009;
    case ErrorCode::ShuttingDown:
        return 1001;
    case ErrorCode::StartFailed:
    case ErrorCode::Internal:
        return 1011;
    case ErrorCode::ResumeUnsupported:
    case ErrorCode::Timeout:
    case ErrorCode::Replaced:
    case ErrorCode::RateLimited:
        return 1008;
    }
    return 1011;
}

ServerSession::ServerSession(SessionConfig config, SessionHost &host)
    : config_(std::move(config))
    , host_(host) {}

ServerSession::State ServerSession::state() const noexcept {
    return state_;
}
std::uint64_t ServerSession::sentOffset() const noexcept {
    return sent_;
}
std::uint64_t ServerSession::ackedOffset() const noexcept {
    return acked_;
}

std::size_t ServerSession::outputBudget() const noexcept {
    if (state_ != State::Running && state_ != State::Closing) {
        return 0;
    }
    const auto unacked = sent_ - acked_;
    return unacked >= config_.outputWindow
               ? 0
               : std::size_t(config_.outputWindow - unacked);
}

void ServerSession::sendMessage(const ServerMessage &message) {
    std::string bytes;
    if (encode(message, bytes)) {
        host_.send(std::move(bytes));
    }
}

void ServerSession::fail(ErrorCode code, const char *reason) {
    if (state_ == State::Closed) {
        return;
    }
    sendMessage(Error{code, reason});
    state_ = State::Closed;
    if (processStarted_ && !processExited_) {
        host_.terminateProcess();
    }
    host_.closeTransport(closeCodeFor(code));
}

void ServerSession::start(Time now) {
    if (state_ != State::Idle) {
        return;
    }
    state_ = State::AwaitHello;
    helloDeadline_ = now + config_.helloTimeout;
    lastMessage_ = now;
    rateWindowStart_ = now;
}

void ServerSession::onMessage(std::string_view bytes, Time now) {
    if (state_ == State::Idle || state_ == State::Closed) {
        return;
    }
    // Per-connection message rate, a fixed window: cheap and good enough to
    // bound the CPU a client can make us spend.
    if (now - rateWindowStart_ >= config_.rateWindow) {
        rateWindowStart_ = now;
        messagesInWindow_ = 0;
    }
    if (++messagesInWindow_ > config_.maxMessagesPerWindow) {
        fail(ErrorCode::RateLimited, "too many messages");
        return;
    }
    const auto decoded = decodeClient(bytes);
    if (!decoded) {
        fail(decoded.error == DecodeError::TooLarge ? ErrorCode::LimitExceeded
                                                    : ErrorCode::BadMessage,
             toString(decoded.error));
        return;
    }
    lastMessage_ = now;
    if (state_ == State::Closing) {
        return; // The client closed; late messages are ignored.
    }
    if (state_ == State::AwaitHello &&
        !std::holds_alternative<Hello>(*decoded.message)) {
        fail(ErrorCode::BadMessage, "expected Hello");
        return;
    }
    std::visit([&](const auto &message) { handle(message, now); },
               *decoded.message);
}

void ServerSession::handle(const Hello &message, Time now) {
    if (state_ != State::AwaitHello) {
        fail(ErrorCode::BadMessage, "unexpected Hello");
        return;
    }
    if (message.version == 0) {
        fail(ErrorCode::UnsupportedVersion, "no common protocol version");
        return;
    }
    if (!message.sessionId.empty() || message.resumeOffset != 0) {
        fail(ErrorCode::ResumeUnsupported, "resume is not supported");
        return;
    }
    if (!host_.startProcess(message.columns, message.rows)) {
        fail(ErrorCode::StartFailed, "could not start the program");
        return;
    }
    processStarted_ = true;
    state_ = State::Running;
    lastResizeApplied_ = now;
    resizeSecondStart_ = now;
    Welcome welcome;
    welcome.version = std::min(message.version, currentVersion);
    welcome.sessionId = config_.sessionId;
    sendMessage(welcome);
}

void ServerSession::handle(const Input &message, Time) {
    if (!host_.writeInput(message.data)) {
        fail(ErrorCode::LimitExceeded, "input queue is full");
    }
}

void ServerSession::applyResize(Time now) {
    if (!pendingResize_) {
        return;
    }
    const auto resize = *pendingResize_;
    pendingResize_.reset();
    lastResizeApplied_ = now;
    host_.resizeProcess(
        resize.columns, resize.rows, resize.pixelWidth, resize.pixelHeight);
}

void ServerSession::handle(const Resize &message, Time now) {
    if (now - resizeSecondStart_ >= std::chrono::seconds(1)) {
        resizeSecondStart_ = now;
        resizesInSecond_ = 0;
    }
    if (++resizesInSecond_ > config_.resizeHardLimit) {
        fail(ErrorCode::RateLimited, "too many resizes");
        return;
    }
    pendingResize_ = message; // Later requests replace earlier ones.
    if (now - lastResizeApplied_ >= config_.resizeInterval) {
        applyResize(now);
    }
}

void ServerSession::handle(const Ack &message, Time now) {
    if (message.consumedOffset < acked_ || message.consumedOffset > sent_) {
        fail(ErrorCode::BadMessage, "invalid acknowledgement");
        return;
    }
    acked_ = message.consumedOffset;
    updateStall(now);
}

void ServerSession::handle(const Ping &message, Time) {
    sendMessage(Pong{message.nonce});
}

void ServerSession::handle(const Close &, Time) {
    state_ = State::Closing;
    pendingResize_.reset();
    if (processStarted_ && !processExited_) {
        host_.terminateProcess();
    }
}

void ServerSession::updateStall(Time now) {
    if (outputBudget() == 0 && state_ != State::Closed) {
        if (!stallSince_) {
            stallSince_ = now;
        }
    }
    else {
        stallSince_.reset();
    }
}

void ServerSession::onProcessOutput(std::string_view bytes, Time now) {
    if (state_ != State::Running && state_ != State::Closing) {
        return;
    }
    while (!bytes.empty()) {
        const auto size = std::min(bytes.size(), maxOutputData);
        sendMessage(Output{sent_, std::string(bytes.substr(0, size))});
        sent_ += size;
        bytes.remove_prefix(size);
    }
    updateStall(now);
}

void ServerSession::onProcessExit(int code, int signal, Time) {
    processExited_ = true;
    if (state_ != State::Running && state_ != State::Closing) {
        return;
    }
    sendMessage(Exit{code, signal});
    state_ = State::Closed;
    host_.closeTransport(1000);
}

void ServerSession::onTransportClosed(Time) {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    if (processStarted_ && !processExited_) {
        host_.terminateProcess();
    }
}

void ServerSession::shutdown(Time) {
    fail(ErrorCode::ShuttingDown, "server is shutting down");
}

std::optional<Time> ServerSession::nextDeadline() const noexcept {
    std::optional<Time> result;
    const auto consider = [&](Time time) {
        if (!result || time < *result) {
            result = time;
        }
    };
    if (state_ == State::AwaitHello) {
        consider(helloDeadline_);
    }
    if (state_ == State::Running) {
        consider(lastMessage_ + config_.idleTimeout);
        if (pendingResize_) {
            consider(lastResizeApplied_ + config_.resizeInterval);
        }
        if (stallSince_) {
            consider(*stallSince_ + config_.ackStallTimeout);
        }
    }
    return result;
}

void ServerSession::tick(Time now) {
    if (state_ == State::AwaitHello && now >= helloDeadline_) {
        fail(ErrorCode::Timeout, "no Hello received");
        return;
    }
    if (state_ != State::Running) {
        return;
    }
    if (now - lastMessage_ >= config_.idleTimeout) {
        fail(ErrorCode::Timeout, "idle timeout");
        return;
    }
    if (stallSince_ && now - *stallSince_ >= config_.ackStallTimeout) {
        fail(ErrorCode::Timeout, "client stopped acknowledging output");
        return;
    }
    if (pendingResize_ && now - lastResizeApplied_ >= config_.resizeInterval) {
        applyResize(now);
    }
}

} // namespace protocol
