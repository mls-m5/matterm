#include "message.h"

#include <type_traits>

namespace protocol {
namespace {

enum Type : std::uint8_t {
    typeHello = 0x01,
    typeInput = 0x02,
    typeResize = 0x03,
    typeAck = 0x04,
    typePing = 0x05,
    typeClose = 0x07,
    typeWelcome = 0x81,
    typeOutput = 0x82,
    typeExit = 0x83,
    typePong = 0x84,
    typeError = 0x85
};

bool validDimension(std::uint16_t value) {
    return value >= 1 && value <= maxDimension;
}

// --- Writing -------------------------------------------------------------

void put8(std::string &out, std::uint8_t value) {
    out.push_back(char(value));
}
void put16(std::string &out, std::uint16_t value) {
    put8(out, std::uint8_t(value >> 8));
    put8(out, std::uint8_t(value));
}
void put32(std::string &out, std::uint32_t value) {
    put16(out, std::uint16_t(value >> 16));
    put16(out, std::uint16_t(value));
}
void put64(std::string &out, std::uint64_t value) {
    put32(out, std::uint32_t(value >> 32));
    put32(out, std::uint32_t(value));
}
void putString(std::string &out, const std::string &value) {
    put16(out, std::uint16_t(value.size()));
    out += value;
}

bool validString(const std::string &value, std::size_t limit) {
    return value.size() <= limit && validUtf8(value);
}

bool valid(const Hello &m) {
    return validDimension(m.columns) && validDimension(m.rows) &&
           validString(m.sessionId, maxSessionIdSize);
}
bool valid(const Input &m) {
    return !m.data.empty() && m.data.size() <= maxInputData;
}
bool valid(const Resize &m) {
    return validDimension(m.columns) && validDimension(m.rows);
}
bool valid(const Welcome &m) {
    return validString(m.sessionId, maxSessionIdSize);
}
bool valid(const Output &m) {
    return !m.data.empty() && m.data.size() <= maxOutputData;
}
bool valid(const Error &m) {
    const auto code = std::uint16_t(m.code);
    return code >= 1 && code <= 10 && validString(m.reason, maxReasonSize);
}
template <typename T>
bool valid(const T &) {
    return true;
}

void write(std::string &out, const Hello &m) {
    put8(out, typeHello);
    put16(out, m.version);
    put16(out, m.columns);
    put16(out, m.rows);
    put64(out, m.resumeOffset);
    putString(out, m.sessionId);
    put32(out, m.features);
}
void write(std::string &out, const Input &m) {
    put8(out, typeInput);
    out += m.data;
}
void write(std::string &out, const Resize &m) {
    put8(out, typeResize);
    put16(out, m.columns);
    put16(out, m.rows);
    put16(out, m.pixelWidth);
    put16(out, m.pixelHeight);
}
void write(std::string &out, const Ack &m) {
    put8(out, typeAck);
    put64(out, m.consumedOffset);
}
void write(std::string &out, const Ping &m) {
    put8(out, typePing);
    put32(out, m.nonce);
}
void write(std::string &out, const Close &) {
    put8(out, typeClose);
}
void write(std::string &out, const Welcome &m) {
    put8(out, typeWelcome);
    put16(out, m.version);
    putString(out, m.sessionId);
    put64(out, m.startOffset);
    put8(out, m.flags);
    put32(out, m.features);
}
void write(std::string &out, const Output &m) {
    put8(out, typeOutput);
    put64(out, m.offset);
    out += m.data;
}
void write(std::string &out, const Exit &m) {
    put8(out, typeExit);
    put32(out, std::uint32_t(m.code));
    put32(out, std::uint32_t(m.signal));
}
void write(std::string &out, const Pong &m) {
    put8(out, typePong);
    put32(out, m.nonce);
}
void write(std::string &out, const Error &m) {
    put8(out, typeError);
    put16(out, std::uint16_t(m.code));
    putString(out, m.reason);
}

template <typename Variant>
bool encodeVariant(const Variant &message, std::string &out) {
    return std::visit(
        [&](const auto &m) {
            if (!valid(m)) {
                return false;
            }
            write(out, m);
            return true;
        },
        message);
}

// --- Reading -------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::string_view bytes)
        : bytes_(bytes) {}

    template <typename T>
    bool get(T &value) {
        static_assert(std::is_unsigned_v<T>);
        if (bytes_.size() - pos_ < sizeof(T)) {
            return false;
        }
        value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value =
                T((std::uint64_t(value) << 8) | std::uint8_t(bytes_[pos_++]));
        }
        return true;
    }
    bool getSigned(std::int32_t &value) {
        std::uint32_t raw = 0;
        if (!get(raw)) {
            return false;
        }
        value = std::int32_t(raw);
        return true;
    }
    // Reads a length-prefixed string; `error` is set on failure.
    bool getString(std::string &value, std::size_t limit, DecodeError &error) {
        std::uint16_t length = 0;
        if (!get(length)) {
            error = DecodeError::Truncated;
            return false;
        }
        if (bytes_.size() - pos_ < length) {
            error = DecodeError::Truncated;
            return false;
        }
        if (length > limit) {
            error = DecodeError::TooLarge;
            return false;
        }
        const auto text = bytes_.substr(pos_, length);
        if (!validUtf8(text)) {
            error = DecodeError::BadString;
            return false;
        }
        value.assign(text);
        pos_ += length;
        return true;
    }
    std::string_view rest() {
        const auto result = bytes_.substr(pos_);
        pos_ = bytes_.size();
        return result;
    }
    bool atEnd() const {
        return pos_ == bytes_.size();
    }

private:
    std::string_view bytes_;
    std::size_t pos_ = 0;
};

// Each reader fills `m`, or returns the error. Trailing bytes are checked by
// the caller for fixed layouts; `Input`/`Output` consume the remainder.
DecodeError read(Reader &r, Hello &m) {
    auto error = DecodeError::Truncated;
    if (!r.get(m.version) || !r.get(m.columns) || !r.get(m.rows) ||
        !r.get(m.resumeOffset) ||
        !r.getString(m.sessionId, maxSessionIdSize, error) ||
        !r.get(m.features)) {
        return error;
    }
    return validDimension(m.columns) && validDimension(m.rows)
               ? DecodeError::None
               : DecodeError::OutOfRange;
}
DecodeError read(Reader &r, Input &m) {
    m.data.assign(r.rest());
    if (m.data.empty()) {
        return DecodeError::OutOfRange;
    }
    return DecodeError::None; // Size was bounded by the message limit.
}
DecodeError read(Reader &r, Resize &m) {
    if (!r.get(m.columns) || !r.get(m.rows) || !r.get(m.pixelWidth) ||
        !r.get(m.pixelHeight)) {
        return DecodeError::Truncated;
    }
    return validDimension(m.columns) && validDimension(m.rows)
               ? DecodeError::None
               : DecodeError::OutOfRange;
}
DecodeError read(Reader &r, Ack &m) {
    return r.get(m.consumedOffset) ? DecodeError::None : DecodeError::Truncated;
}
DecodeError read(Reader &r, Ping &m) {
    return r.get(m.nonce) ? DecodeError::None : DecodeError::Truncated;
}
DecodeError read(Reader &, Close &) {
    return DecodeError::None;
}
DecodeError read(Reader &r, Welcome &m) {
    auto error = DecodeError::Truncated;
    if (!r.get(m.version) ||
        !r.getString(m.sessionId, maxSessionIdSize, error) ||
        !r.get(m.startOffset) || !r.get(m.flags) || !r.get(m.features)) {
        return error;
    }
    return DecodeError::None;
}
DecodeError read(Reader &r, Output &m) {
    if (!r.get(m.offset)) {
        return DecodeError::Truncated;
    }
    m.data.assign(r.rest());
    return m.data.empty() ? DecodeError::OutOfRange : DecodeError::None;
}
DecodeError read(Reader &r, Exit &m) {
    return r.getSigned(m.code) && r.getSigned(m.signal)
               ? DecodeError::None
               : DecodeError::Truncated;
}
DecodeError read(Reader &r, Pong &m) {
    return r.get(m.nonce) ? DecodeError::None : DecodeError::Truncated;
}
DecodeError read(Reader &r, Error &m) {
    std::uint16_t code = 0;
    auto error = DecodeError::Truncated;
    if (!r.get(code) || !r.getString(m.reason, maxReasonSize, error)) {
        return error;
    }
    if (code < 1 || code > 10) {
        return DecodeError::OutOfRange;
    }
    m.code = ErrorCode(code);
    return DecodeError::None;
}

template <typename Message, typename T>
Decoded<Message> decodeAs(Reader &reader) {
    T message;
    const auto error = read(reader, message);
    if (error != DecodeError::None) {
        return {std::nullopt, error};
    }
    if (!reader.atEnd()) {
        return {std::nullopt, DecodeError::Trailing};
    }
    return {Message(std::move(message)), DecodeError::None};
}

} // namespace

bool validUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = std::uint8_t(text[i]);
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code = 0;
        if (b0 < 0x80) {
            ++i;
            continue;
        }
        if (b0 >= 0xc2 && b0 <= 0xdf) {
            length = 2;
            minimum = 0x80;
            code = b0 & 0x1f;
        }
        else if (b0 >= 0xe0 && b0 <= 0xef) {
            length = 3;
            minimum = 0x800;
            code = b0 & 0x0f;
        }
        else if (b0 >= 0xf0 && b0 <= 0xf4) {
            length = 4;
            minimum = 0x10000;
            code = b0 & 0x07;
        }
        else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto b = std::uint8_t(text[i + k]);
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (b & 0x3f);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool encode(const ClientMessage &message, std::string &out) {
    std::string bytes;
    if (!encodeVariant(message, bytes) || bytes.size() > maxMessageSize) {
        return false;
    }
    out += bytes;
    return true;
}
bool encode(const ServerMessage &message, std::string &out) {
    std::string bytes;
    if (!encodeVariant(message, bytes) || bytes.size() > maxMessageSize) {
        return false;
    }
    out += bytes;
    return true;
}

Decoded<ClientMessage> decodeClient(std::string_view bytes) {
    if (bytes.size() > maxMessageSize) {
        return {std::nullopt, DecodeError::TooLarge};
    }
    if (bytes.empty()) {
        return {std::nullopt, DecodeError::Truncated};
    }
    const auto type = std::uint8_t(bytes[0]);
    Reader reader(bytes.substr(1));
    switch (type) {
    case typeHello:
        return decodeAs<ClientMessage, Hello>(reader);
    case typeInput:
        return decodeAs<ClientMessage, Input>(reader);
    case typeResize:
        return decodeAs<ClientMessage, Resize>(reader);
    case typeAck:
        return decodeAs<ClientMessage, Ack>(reader);
    case typePing:
        return decodeAs<ClientMessage, Ping>(reader);
    case typeClose:
        return decodeAs<ClientMessage, Close>(reader);
    default:
        return {std::nullopt,
                type >= 0x81 ? DecodeError::WrongDirection
                             : DecodeError::UnknownType};
    }
}

Decoded<ServerMessage> decodeServer(std::string_view bytes) {
    if (bytes.size() > maxMessageSize) {
        return {std::nullopt, DecodeError::TooLarge};
    }
    if (bytes.empty()) {
        return {std::nullopt, DecodeError::Truncated};
    }
    const auto type = std::uint8_t(bytes[0]);
    Reader reader(bytes.substr(1));
    switch (type) {
    case typeWelcome:
        return decodeAs<ServerMessage, Welcome>(reader);
    case typeOutput:
        return decodeAs<ServerMessage, Output>(reader);
    case typeExit:
        return decodeAs<ServerMessage, Exit>(reader);
    case typePong:
        return decodeAs<ServerMessage, Pong>(reader);
    case typeError:
        return decodeAs<ServerMessage, Error>(reader);
    default:
        return {std::nullopt,
                type >= 0x01 && type <= 0x07 && type != 0x06
                    ? DecodeError::WrongDirection
                    : DecodeError::UnknownType};
    }
}

const char *toString(DecodeError error) noexcept {
    switch (error) {
    case DecodeError::None:
        return "none";
    case DecodeError::Truncated:
        return "truncated";
    case DecodeError::Trailing:
        return "trailing bytes";
    case DecodeError::UnknownType:
        return "unknown message type";
    case DecodeError::WrongDirection:
        return "message in the wrong direction";
    case DecodeError::BadString:
        return "invalid UTF-8";
    case DecodeError::TooLarge:
        return "too large";
    case DecodeError::OutOfRange:
        return "value out of range";
    }
    return "unknown";
}

const char *toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::BadMessage:
        return "BadMessage";
    case ErrorCode::UnsupportedVersion:
        return "UnsupportedVersion";
    case ErrorCode::ResumeUnsupported:
        return "ResumeUnsupported";
    case ErrorCode::LimitExceeded:
        return "LimitExceeded";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::Replaced:
        return "Replaced";
    case ErrorCode::ShuttingDown:
        return "ShuttingDown";
    case ErrorCode::RateLimited:
        return "RateLimited";
    case ErrorCode::StartFailed:
        return "StartFailed";
    case ErrorCode::Internal:
        return "Internal";
    }
    return "unknown";
}

} // namespace protocol
