#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// matterm-tty wire protocol, version 1. See docs/PROTOCOL.md for the format.
// Pure data and codec: no I/O, no clocks, no SDL, no terminal model.
namespace protocol {

constexpr std::uint16_t currentVersion = 1;
constexpr std::size_t maxMessageSize = 16384; // Including the type byte.
constexpr std::size_t maxInputData = maxMessageSize - 1;
constexpr std::size_t maxOutputData = maxMessageSize - 1 - 8;
constexpr std::uint16_t maxDimension = 1000;
constexpr std::size_t maxSessionIdSize = 64;
constexpr std::size_t maxReasonSize = 256;

enum class ErrorCode : std::uint16_t {
    BadMessage = 1,
    UnsupportedVersion = 2,
    ResumeUnsupported = 3,
    LimitExceeded = 4,
    Timeout = 5,
    Replaced = 6,
    ShuttingDown = 7,
    RateLimited = 8,
    StartFailed = 9,
    Internal = 10
};

// Client to server.
struct Hello {
    std::uint16_t version = currentVersion;
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;
    std::uint64_t resumeOffset = 0;
    std::string sessionId;
    std::uint32_t features = 0;
};
struct Input {
    std::string data;
};
struct Resize {
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;
    std::uint16_t pixelWidth = 0; // 0 means unknown.
    std::uint16_t pixelHeight = 0;
};
struct Ack {
    std::uint64_t consumedOffset = 0;
};
struct Ping {
    std::uint32_t nonce = 0;
};
struct Close {};

// Server to client.
struct Welcome {
    std::uint16_t version = currentVersion;
    std::string sessionId;
    std::uint64_t startOffset = 0;
    std::uint8_t flags = 0;
    std::uint32_t features = 0;
};
struct Output {
    std::uint64_t offset = 0;
    std::string data;
};
struct Exit {
    std::int32_t code = 0;
    std::int32_t signal = 0;
};
struct Pong {
    std::uint32_t nonce = 0;
};
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string reason;
};

using ClientMessage = std::variant<Hello, Input, Resize, Ack, Ping, Close>;
using ServerMessage = std::variant<Welcome, Output, Exit, Pong, Error>;

enum class DecodeError {
    None,
    Truncated,      // Missing bytes, or an empty message.
    Trailing,       // Bytes left after a complete layout.
    UnknownType,    // Includes the reserved type 0x06.
    WrongDirection, // A server type received by a server, or the reverse.
    BadString,      // Invalid UTF-8.
    TooLarge,       // Message or string over its limit.
    OutOfRange      // Dimension, data size, or error code out of range.
};

template <typename Message>
struct Decoded {
    std::optional<Message> message;
    DecodeError error = DecodeError::None;

    explicit operator bool() const noexcept {
        return message.has_value();
    }
};

// Appends one message to `out`. Returns false, leaving `out` unchanged, when
// the message violates a protocol limit (the same checks decode applies).
bool encode(const ClientMessage &message, std::string &out);
bool encode(const ServerMessage &message, std::string &out);

// Decode exactly one complete message (one WebSocket message).
Decoded<ClientMessage> decodeClient(std::string_view bytes);
Decoded<ServerMessage> decodeServer(std::string_view bytes);

// Strict UTF-8 validation: rejects overlong forms, surrogates, > U+10FFFF.
bool validUtf8(std::string_view text) noexcept;

const char *toString(DecodeError error) noexcept;
const char *toString(ErrorCode code) noexcept;

} // namespace protocol
