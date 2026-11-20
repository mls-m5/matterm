#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The chat's wire format: one WebSocket text frame is one line, fields
// separated by a single TAB, at most kMaxFrameBytes long (docs/CHAT.md, section
// 3). Parsing is strict and total: a line is either exactly one of the known
// shapes or an error; nothing is repaired. Pure functions, no I/O.
namespace chat {

constexpr std::size_t kMaxFrameBytes = 1024;
constexpr std::uint32_t kMaxBubble = 999999999;

enum class ProtoError {
    None,
    TooLong,
    BadChars, // CR, LF, NUL or another control byte in the frame.
    Unknown,  // An unknown command.
    Fields,   // Wrong number of fields.
    Number,
    Name,
    Text,
};

// Lines a client sends.
struct ClientLine {
    enum class Kind { Hello, Draft, Lock, Nick };
    Kind kind = Kind::Hello;
    std::string name;         // Hello, Nick.
    std::uint32_t bubble = 0; // Draft, Lock: 1..kMaxBubble.
    std::string text;         // Draft: the whole draft, may be empty.
    friend bool operator==(const ClientLine &a, const ClientLine &b) {
        return a.kind == b.kind && a.name == b.name && a.bubble == b.bubble &&
               a.text == b.text;
    }
};
struct ClientParse {
    ProtoError error = ProtoError::None;
    ClientLine line;
};
// What the server does with a frame from a client (validates names and text
// with chattext.h).
ClientParse parseClientLine(std::string_view frame);
std::string formatClientLine(const ClientLine &line);

enum class ErrorCode {
    NameTaken,
    BadName,
    TooLong,
    Rate,
    Full,
    Banned,
    BadLine,
};
const char *errorCodeText(ErrorCode code);

// Lines the server sends.
struct ServerLine {
    enum class Kind {
        Welcome, // name
        Users,   // names
        Hist,    // serial, age, name, text
        Ready,
        Draft,  // name, text
        Gone,   // name
        Typers, // count
        Msg,    // serial, name, text
        Ack,    // bubble, serial, text
        Online, // count
        Join,   // name
        Leave,  // name
        Hide,   // serial
        Notice, // text
        Error,  // code, text
    };
    Kind kind = Kind::Ready;
    std::string name;
    std::vector<std::string> names;
    std::string text;
    std::uint64_t serial = 0;
    std::uint32_t age = 0; // Seconds.
    std::uint32_t bubble = 0;
    std::uint32_t count = 0;
    ErrorCode code = ErrorCode::BadLine;
    friend bool operator==(const ServerLine &a, const ServerLine &b) {
        return a.kind == b.kind && a.name == b.name && a.names == b.names &&
               a.text == b.text && a.serial == b.serial && a.age == b.age &&
               a.bubble == b.bubble && a.count == b.count && a.code == b.code;
    }
};
struct ServerParse {
    ProtoError error = ProtoError::None;
    ServerLine line;
};
// What the client does with a frame from the server. Text fields come back
// sanitized (sanitizeIncoming), so they are safe to show; names must be valid
// names or the line is an error.
ServerParse parseServerLine(std::string_view frame);
std::string formatServerLine(const ServerLine &line);

} // namespace chat
