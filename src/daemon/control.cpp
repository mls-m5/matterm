#include "control.h"

#include "framing.h"
#include "message.h"
#include "utf8decoder.h"

#include <algorithm>

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
    tSetName = 0x48,
    tGetActivity = 0x49,
    tCreateTerminal = 0x4a,
    tEndSession = 0x4b,
    tGetSessionDetails = 0x4c,
    tControlWelcome = 0xc0,
    tSessionList = 0xc1,
    tStatus = 0xc2,
    tShutdownReply = 0xc3,
    tControlError = 0xc4,
    tSessionCreated = 0xc5,
    tSessionInfo = 0xc6,
    tSessionListNamed = 0xc7,
    tSessionInfoNamed = 0xc8,
    tActivity = 0xc9,
    tSessionEnded = 0xca,
    tSessionDetails = 0xcb
};
constexpr std::size_t kMaxId = 64;
constexpr std::size_t kMaxText = 256;

} // namespace

bool validTerminalName(std::string_view name) {
    if (name.size() > kMaxName || !protocol::validUtf8(name)) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c == 0xc2 && i + 1 < name.size() && static_cast<unsigned char>(name[i + 1]) < 0xa0) return false; // C1
    }
    return true;
}

bool validPreviewScalar(char32_t cp) noexcept {
    return cp >= 0x20 && !(cp >= 0x7f && cp <= 0x9f) && cp != 0x061c &&
           cp != 0x200e && cp != 0x200f && !(cp >= 0x2028 && cp <= 0x202e) &&
           !(cp >= 0x2066 && cp <= 0x2069) && cp <= 0x10ffff &&
           !(cp >= 0xd800 && cp <= 0xdfff);
}
bool validPreviewText(std::string_view text) {
    if (text.size() > kMaxPreviewBytes || !protocol::validUtf8(text))
        return false;
    Utf8Decoder decoder;
    const auto decoded = decoder.feed(text);
    return decoded.size() <= kMaxPreviewScalars &&
           std::all_of(decoded.begin(), decoded.end(), validPreviewScalar);
}

namespace {
bool validDetailId(std::string_view id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool validDetailCommand(const std::vector<std::string> &command) {
    if (command.size() > kMaxArguments ||
        (!command.empty() && command.front().empty()))
        return false;
    std::size_t total = 0;
    for (const auto &arg : command) {
        if (arg.size() > kMaxArgument ||
            arg.size() + 1 > kMaxDetailCommand - total ||
            arg.find('\0') != std::string::npos)
            return false;
        total += arg.size() + 1;
    }
    return true;
}

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
bool write(std::string &o, const SetName &m) {
    put8(o, tSetName);
    if (!putString(o, m.id, kMaxId) || !validTerminalName(m.name)) return false;
    return putString(o, m.name, kMaxName);
}
bool write(std::string &o, const CreateTerminal &m) {
    put8(o, tCreateTerminal);
    if (!putString(o, m.cwd, kMaxPath) || m.columns == 0 || m.rows == 0 ||
        m.command.size() > kMaxArguments ||
        (!m.command.empty() && m.command.front().empty()) ||
        !validTerminalName(m.name)) {
        return false;
    }
    put16(o, m.columns);
    put16(o, m.rows);
    put8(o, std::uint8_t(m.command.size()));
    for (const auto &a : m.command) {
        if (a.find('\0') != std::string::npos || !putString(o, a, kMaxArgument))
            return false;
    }
    return putString(o, m.name, kMaxName);
}
bool write(std::string &o, const EndSession &m) {
    put8(o, tEndSession);
    return !m.id.empty() && putString(o, m.id, kMaxId);
}
bool write(std::string &o, const SessionEnded &m) {
    put8(o, tSessionEnded);
    return putString(o, m.id, kMaxId);
}
bool write(std::string &o, const GetStatus &) { put8(o, tGetStatus); return true; }
bool write(std::string &o, const GetActivity &) {
    put8(o, tGetActivity);
    return true;
}
bool write(std::string &o, const Activity &m) {
    put8(o, tActivity);
    put8(o, m.attachedNow ? 1 : 0);
    put32(o, m.secondsSinceClient);
    return true;
}
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
bool write(std::string &o, const GetSessionDetails &m) {
    if (m.ids.empty() || m.ids.size() > kMaxDetailIds)
        return false;
    put8(o, tGetSessionDetails);
    put8(o, std::uint8_t(m.ids.size()));
    put8(o, m.includePreview ? 1 : 0);
    for (std::size_t i = 0; i < m.ids.size(); ++i) {
        if (!validDetailId(m.ids[i]) ||
            std::find(m.ids.begin(),
                      m.ids.begin() + std::ptrdiff_t(i),
                      m.ids[i]) != m.ids.begin() + std::ptrdiff_t(i) ||
            !putString(o, m.ids[i], 32))
            return false;
    }
    return true;
}
bool write(std::string &o, const SessionDetails &m) {
    if (m.processedCount > kMaxDetailIds || m.rows.size() > m.processedCount)
        return false;
    put8(o, tSessionDetails);
    put16(o, m.processedCount);
    put8(o, std::uint8_t(m.rows.size()));
    for (std::size_t i = 0; i < m.rows.size(); ++i) {
        const auto &row = m.rows[i];
        if (!validDetailId(row.id) || !validTerminalName(row.name) ||
            !validDetailCommand(row.command))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (m.rows[j].id == row.id)
                return false;
        if (!putString(o, row.id, 32))
            return false;
        const auto flags =
            std::uint8_t((row.foregroundBusy ? 1 : 0) |
                         (row.foregroundBusy.value_or(false) ? 2 : 0) |
                         (row.outputAgeMs ? 4 : 0) | (row.preview ? 8 : 0));
        put8(o, flags);
        if (!putString(o, row.name, kMaxName) ||
            !putString(o, row.cwd, kMaxPath))
            return false;
        put8(o, std::uint8_t(row.command.size()));
        for (const auto &arg : row.command)
            if (!putString(o, arg, kMaxArgument))
                return false;
        if (row.outputAgeMs)
            put64(o, *row.outputAgeMs);
        if (row.preview && (!validPreviewText(*row.preview) ||
                            !putString(o, *row.preview, kMaxPreviewBytes)))
            return false;
        if (o.size() > kMaxFrame)
            return false;
    }
    return true;
}
bool write(std::string &o, const SessionCreated &m) {
    put8(o, tSessionCreated);
    return putString(o, m.id, kMaxId);
}
bool writeInfo(std::string &o, const SessionInfo &m, bool named) {
    if (m.command.size() > kMaxArguments) return false;
    put8(o, named ? tSessionInfoNamed : tSessionInfo);
    if (!putString(o, m.id, kMaxId) || !putString(o, m.cwd, kMaxPath)) return false;
    put8(o, m.busy ? 1 : 0);
    put8(o, std::uint8_t(m.command.size()));
    for (const auto &a : m.command) {
        if (!putString(o, a, kMaxArgument)) return false;
    }
    return !named || (validTerminalName(m.name) && putString(o, m.name, kMaxName));
}
bool write(std::string &o, const ControlWelcome &m) {
    put8(o, tControlWelcome);
    put16(o, m.version);
    put32(o, m.pid);
    put32(o, m.features);
    return putString(o, m.build, kMaxText);
}
bool writeList(std::string &o, const SessionList &m, bool named) {
    if (m.sessions.size() > kMaxListed) return false;
    put8(o, named ? tSessionListNamed : tSessionList);
    put16(o, std::uint16_t(m.sessions.size()));
    for (const auto &s : m.sessions) {
        if (!putString(o, s.id, kMaxId)) return false;
        put8(o, std::uint8_t((s.attached ? 1 : 0) | (s.shared ? 2 : 0)));
        put64(o, s.outputEnd);
        if (named && !(validTerminalName(s.name) && putString(o, s.name, kMaxName))) return false;
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

template <class M>
bool writeFor(std::string &o, const M &m, std::uint16_t) { return write(o, m); }
bool writeFor(std::string &o, const SessionList &m, std::uint16_t version) { return writeList(o, m, version >= 3); }
bool writeFor(std::string &o, const SessionInfo &m, std::uint16_t version) { return writeInfo(o, m, version >= 3); }

template <class Variant>
bool encodeVariant(const Variant &message, std::string &out, std::uint16_t version = kControlVersion) {
    std::string tmp;
    const bool ok = std::visit([&](const auto &m) { return writeFor(tmp, m, version); }, message);
    if (!ok) return false;
    out += tmp;
    return true;
}

} // namespace

bool encode(const ControlRequest &message, std::string &out) { return encodeVariant(message, out); }
bool encode(const ControlReply &message, std::string &out, std::uint16_t peerVersion) {
    return encodeVariant(message, out, peerVersion);
}

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
    case tGetActivity:
        return r.done() ? std::optional<ControlRequest>(GetActivity{})
                        : std::nullopt;
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
    case tCreateTerminal: {
        CreateTerminal m;
        std::uint8_t n;
        if (!r.get(m.cwd, kMaxPath) || !r.get(m.columns) || !r.get(m.rows) ||
            m.columns == 0 || m.rows == 0 ||
            m.columns > protocol::maxDimension ||
            m.rows > protocol::maxDimension || !r.get(n) || n > kMaxArguments) {
            return std::nullopt;
        }
        for (std::uint8_t i = 0; i < n; ++i) {
            std::string a;
            if (!r.get(a, kMaxArgument) || a.find('\0') != std::string::npos ||
                (i == 0 && a.empty()))
                return std::nullopt;
            m.command.push_back(std::move(a));
        }
        if (!r.get(m.name, kMaxName) || !validTerminalName(m.name) || !r.done())
            return std::nullopt;
        return m;
    }
    case tEndSession: {
        EndSession m;
        if (!r.get(m.id, kMaxId) || m.id.empty() || !r.done())
            return std::nullopt;
        return m;
    }
    case tSetName: {
        SetName m;
        if (!r.get(m.id, kMaxId) || m.id.empty() || !r.get(m.name, kMaxName) || !validTerminalName(m.name) ||
            !r.done()) {
            return std::nullopt;
        }
        return m;
    }
    case tGetSessionDetails: {
        GetSessionDetails m;
        std::uint8_t count;
        if (!r.get(count) || count == 0 || count > kMaxDetailIds ||
            !r.getBool(m.includePreview))
            return std::nullopt;
        for (std::uint8_t i = 0; i < count; ++i) {
            std::string id;
            if (!r.get(id, 32) || !validDetailId(id) ||
                std::find(m.ids.begin(), m.ids.end(), id) != m.ids.end())
                return std::nullopt;
            m.ids.push_back(std::move(id));
        }
        return r.done() ? std::optional<ControlRequest>(std::move(m))
                        : std::nullopt;
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
    case tSessionList:
    case tSessionListNamed: {
        const bool named = type == tSessionListNamed;
        SessionList m;
        std::uint16_t n;
        if (!r.get(n) || n > kMaxListed) return std::nullopt;
        for (std::uint16_t i = 0; i < n; ++i) {
            SessionEntry e;
            std::uint8_t flags;
            if (!r.get(e.id, kMaxId) || !r.get(flags) || (flags & ~3u) || !r.get(e.outputEnd)) return std::nullopt;
            if (named && (!r.get(e.name, kMaxName) || !validTerminalName(e.name))) return std::nullopt;
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
    case tActivity: {
        Activity m;
        if (!r.getBool(m.attachedNow) || !r.get(m.secondsSinceClient) ||
            !r.done())
            return std::nullopt;
        return m;
    }
    case tSessionEnded: {
        SessionEnded m;
        if (!r.get(m.id, kMaxId) || !r.done())
            return std::nullopt;
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
    case tSessionDetails: {
        SessionDetails m;
        std::uint8_t count;
        if (bytes.size() > kMaxFrame || !r.get(m.processedCount) ||
            m.processedCount > kMaxDetailIds || !r.get(count) ||
            count > m.processedCount)
            return std::nullopt;
        for (std::uint8_t i = 0; i < count; ++i) {
            SessionDetail row;
            std::uint8_t flags, argc;
            if (!r.get(row.id, 32) || !validDetailId(row.id) || !r.get(flags) ||
                (flags & ~15u) || ((flags & 2u) && !(flags & 1u)) ||
                !r.get(row.name, kMaxName) || !validTerminalName(row.name) ||
                !r.get(row.cwd, kMaxPath) || !r.get(argc) ||
                argc > kMaxArguments)
                return std::nullopt;
            for (const auto &previous : m.rows)
                if (previous.id == row.id)
                    return std::nullopt;
            for (std::uint8_t j = 0; j < argc; ++j) {
                std::string arg;
                if (!r.get(arg, kMaxArgument))
                    return std::nullopt;
                row.command.push_back(std::move(arg));
            }
            if (!validDetailCommand(row.command))
                return std::nullopt;
            if (flags & 1u)
                row.foregroundBusy = (flags & 2u) != 0;
            if (flags & 4u) {
                std::uint64_t age;
                if (!r.get(age))
                    return std::nullopt;
                row.outputAgeMs = age;
            }
            if (flags & 8u) {
                std::string text;
                if (!r.get(text, kMaxPreviewBytes) || !validPreviewText(text))
                    return std::nullopt;
                row.preview = std::move(text);
            }
            m.rows.push_back(std::move(row));
        }
        return r.done() ? std::optional<ControlReply>(std::move(m))
                        : std::nullopt;
    }
    case tSessionInfo:
    case tSessionInfoNamed: {
        const bool named = type == tSessionInfoNamed;
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
        if (named && (!r.get(m.name, kMaxName) || !validTerminalName(m.name))) return std::nullopt;
        return r.done() ? std::optional<ControlReply>(std::move(m)) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

} // namespace mtd
