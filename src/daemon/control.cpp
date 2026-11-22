#include "control.h"

#include "message.h"

namespace mtd {
namespace {

enum Type : std::uint8_t {
    tControlHello = 0x40,
    tListSessions = 0x41,
    tSetShared = 0x42,
    tGetStatus = 0x43,
    tShutdown = 0x44,
    tCreateSession = 0x45,
    tGetSessionInfo = 0x46,
    tExposedHello = 0x47,
    tControlWelcome = 0xc0,
    tSessionList = 0xc1,
    tStatus = 0xc2,
    tShutdownReply = 0xc3,
    tControlError = 0xc4,
    tSessionCreated = 0xc5,
    tSessionInfo = 0xc6
};
constexpr std::size_t kMaxId = 64;
constexpr std::size_t kMaxText = 256;

void put8(std::string &o, std::uint8_t v) { o.push_back(char(v)); }
void put16(std::string &o, std::uint16_t v) { put8(o, std::uint8_t(v >> 8)); put8(o, std::uint8_t(v)); }
void put32(std::string &o, std::uint32_t v) { put16(o, std::uint16_t(v >> 16)); put16(o, std::uint16_t(v)); }
void put64(std::string &o, std::uint64_t v) { put32(o, std::uint32_t(v >> 32)); put32(o, std::uint32_t(v)); }
bool putString(std::string &o, const std::string &s, std::size_t limit) {
    if (s.size() > limit || !protocol::validUtf8(s)) return false;
    put16(o, std::uint16_t(s.size()));
    o += s;
    return true;
}

class Reader {
public:
    explicit Reader(std::string_view b) : b_(b) {}
    bool get(std::uint8_t &v) { if (b_.empty()) return false; v = std::uint8_t(b_[0]); b_.remove_prefix(1); return true; }
    bool get(std::uint16_t &v) { std::uint8_t a, c; if (!get(a) || !get(c)) return false; v = std::uint16_t(a << 8 | c); return true; }
    bool get(std::uint32_t &v) { std::uint16_t a, c; if (!get(a) || !get(c)) return false; v = std::uint32_t(a) << 16 | c; return true; }
    bool get(std::uint64_t &v) { std::uint32_t a, c; if (!get(a) || !get(c)) return false; v = std::uint64_t(a) << 32 | c; return true; }
    bool get(std::string &s, std::size_t limit) {
        std::uint16_t n;
        if (!get(n) || n > limit || b_.size() < n) return false;
        s.assign(b_.substr(0, n));
        b_.remove_prefix(n);
        return protocol::validUtf8(s);
    }
    bool getBool(bool &v) { std::uint8_t x; if (!get(x) || x > 1) return false; v = x == 1; return true; }
    bool done() const { return b_.empty(); }
private:
    std::string_view b_;
};

bool write(std::string &o, const ControlHello &m) { put8(o, tControlHello); put16(o, m.version); return true; }
bool write(std::string &o, const ExposedHello &m) { put8(o, tExposedHello); put16(o, m.version); return true; }
bool write(std::string &o, const ListSessions &) { put8(o, tListSessions); return true; }
bool write(std::string &o, const SetShared &m) {
    put8(o, tSetShared);
    if (!putString(o, m.id, kMaxId)) return false;
    put8(o, m.shared ? 1 : 0);
    return true;
}
bool write(std::string &o, const GetStatus &) { put8(o, tGetStatus); return true; }
bool write(std::string &o, const Shutdown &m) { put8(o, tShutdown); put32(o, m.expectedSessions); return true; }
bool write(std::string &o, const CreateSession &m) {
    put8(o, tCreateSession);
    if (!putString(o, m.cwd, kMaxPath)) return false;
    put16(o, m.columns);
    put16(o, m.rows);
    return true;
}
bool write(std::string &o, const GetSessionInfo &m) {
    put8(o, tGetSessionInfo);
    return putString(o, m.id, kMaxId);
}
bool write(std::string &o, const SessionCreated &m) {
    put8(o, tSessionCreated);
    return putString(o, m.id, kMaxId);
}
bool write(std::string &o, const SessionInfo &m) {
    if (m.command.size() > kMaxArguments) return false;
    put8(o, tSessionInfo);
    if (!putString(o, m.id, kMaxId) || !putString(o, m.cwd, kMaxPath)) return false;
    put8(o, m.busy ? 1 : 0);
    put8(o, std::uint8_t(m.command.size()));
    for (const auto &a : m.command) {
        if (!putString(o, a, kMaxArgument)) return false;
    }
    return true;
}
bool write(std::string &o, const ControlWelcome &m) {
    put8(o, tControlWelcome);
    put16(o, m.version);
    put32(o, m.pid);
    put32(o, m.features);
    return putString(o, m.build, kMaxText);
}
bool write(std::string &o, const SessionList &m) {
    if (m.sessions.size() > kMaxListed) return false;
    put8(o, tSessionList);
    put16(o, std::uint16_t(m.sessions.size()));
    for (const auto &s : m.sessions) {
        if (!putString(o, s.id, kMaxId)) return false;
        put8(o, std::uint8_t((s.attached ? 1 : 0) | (s.shared ? 2 : 0)));
        put64(o, s.outputEnd);
    }
    return true;
}
bool write(std::string &o, const Status &m) {
    put8(o, tStatus);
    put32(o, m.pid); put32(o, m.sessions); put32(o, m.attached); put32(o, m.shared); put32(o, m.clients);
    put64(o, m.uptimeMs);
    put32(o, m.idleExitSeconds);
    return true;
}
bool write(std::string &o, const ShutdownReply &m) { put8(o, tShutdownReply); put8(o, m.done ? 1 : 0); put32(o, m.sessions); return true; }
bool write(std::string &o, const ControlError &m) {
    put8(o, tControlError);
    put16(o, std::uint16_t(m.code));
    return putString(o, m.reason, kMaxText);
}

template <class Variant>
bool encodeVariant(const Variant &message, std::string &out) {
    std::string tmp;
    const bool ok = std::visit([&tmp](const auto &m) { return write(tmp, m); }, message);
    if (!ok) return false;
    out += tmp;
    return true;
}

} // namespace

bool encode(const ControlRequest &message, std::string &out) { return encodeVariant(message, out); }
bool encode(const ControlReply &message, std::string &out) { return encodeVariant(message, out); }

std::optional<ControlRequest> decodeRequest(std::string_view bytes) {
    Reader r(bytes);
    std::uint8_t type;
    if (!r.get(type)) return std::nullopt;
    switch (type) {
    case tControlHello: {
        ControlHello m;
        if (!r.get(m.version) || m.version == 0 || !r.done()) return std::nullopt;
        return m;
    }
    case tExposedHello: {
        ExposedHello m;
        if (!r.get(m.version) || m.version == 0 || !r.done()) return std::nullopt;
        return m;
    }
    case tListSessions:
        return r.done() ? std::optional<ControlRequest>(ListSessions{}) : std::nullopt;
    case tSetShared: {
        SetShared m;
        if (!r.get(m.id, kMaxId) || m.id.empty() || !r.getBool(m.shared) || !r.done()) return std::nullopt;
        return m;
    }
    case tGetStatus:
        return r.done() ? std::optional<ControlRequest>(GetStatus{}) : std::nullopt;
    case tShutdown: {
        Shutdown m;
        if (!r.get(m.expectedSessions) || !r.done()) return std::nullopt;
        return m;
    }
    case tCreateSession: {
        CreateSession m;
        if (!r.get(m.cwd, kMaxPath) || !r.get(m.columns) || !r.get(m.rows) || m.columns == 0 || m.rows == 0 ||
            m.columns > protocol::maxDimension || m.rows > protocol::maxDimension || !r.done()) {
            return std::nullopt;
        }
        return m;
    }
    case tGetSessionInfo: {
        GetSessionInfo m;
        if (!r.get(m.id, kMaxId) || m.id.empty() || !r.done()) return std::nullopt;
        return m;
    }
    default:
        return std::nullopt;
    }
}

std::optional<ControlReply> decodeReply(std::string_view bytes) {
    Reader r(bytes);
    std::uint8_t type;
    if (!r.get(type)) return std::nullopt;
    switch (type) {
    case tControlWelcome: {
        ControlWelcome m;
        if (!r.get(m.version) || !r.get(m.pid) || !r.get(m.features) || !r.get(m.build, kMaxText) || !r.done()) {
            return std::nullopt;
        }
        return m;
    }
    case tSessionList: {
        SessionList m;
        std::uint16_t n;
        if (!r.get(n) || n > kMaxListed) return std::nullopt;
        for (std::uint16_t i = 0; i < n; ++i) {
            SessionEntry e;
            std::uint8_t flags;
            if (!r.get(e.id, kMaxId) || !r.get(flags) || (flags & ~3u) || !r.get(e.outputEnd)) return std::nullopt;
            e.attached = (flags & 1) != 0;
            e.shared = (flags & 2) != 0;
            m.sessions.push_back(std::move(e));
        }
        return r.done() ? std::optional<ControlReply>(std::move(m)) : std::nullopt;
    }
    case tStatus: {
        Status m;
        if (!r.get(m.pid) || !r.get(m.sessions) || !r.get(m.attached) || !r.get(m.shared) || !r.get(m.clients) ||
            !r.get(m.uptimeMs) || !r.get(m.idleExitSeconds) || !r.done()) {
            return std::nullopt;
        }
        return m;
    }
    case tShutdownReply: {
        ShutdownReply m;
        if (!r.getBool(m.done) || !r.get(m.sessions) || !r.done()) return std::nullopt;
        return m;
    }
    case tControlError: {
        ControlError m;
        std::uint16_t code;
        if (!r.get(code) || code < 1 || code > 5 || !r.get(m.reason, kMaxText) || !r.done()) return std::nullopt;
        m.code = static_cast<ControlErrorCode>(code);
        return m;
    }
    case tSessionCreated: {
        SessionCreated m;
        if (!r.get(m.id, kMaxId) || m.id.empty() || !r.done()) return std::nullopt;
        return m;
    }
    case tSessionInfo: {
        SessionInfo m;
        std::uint8_t n;
        if (!r.get(m.id, kMaxId) || !r.get(m.cwd, kMaxPath) || !r.getBool(m.busy) || !r.get(n) || n > kMaxArguments) {
            return std::nullopt;
        }
        for (std::uint8_t i = 0; i < n; ++i) {
            std::string a;
            if (!r.get(a, kMaxArgument)) return std::nullopt;
            m.command.push_back(std::move(a));
        }
        return r.done() ? std::optional<ControlReply>(std::move(m)) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

} // namespace mtd
