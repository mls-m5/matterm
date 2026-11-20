#include "chatproto.h"

#include "chattext.h"

#include <array>

namespace chat {
namespace {

constexpr std::size_t kMaxFields = 6;

struct Fields {
    std::array<std::string_view, kMaxFields> part;
    std::size_t count = 0;
    bool tooMany = false;
};

Fields split(std::string_view frame) {
    Fields f;
    std::size_t start = 0;
    while (true) {
        const auto tab = frame.find('\t', start);
        const auto piece = frame.substr(
            start, tab == std::string_view::npos ? tab : tab - start);
        if (f.count == kMaxFields) {
            f.tooMany = true;
            break;
        }
        f.part[f.count++] = piece;
        if (tab == std::string_view::npos) {
            break;
        }
        start = tab + 1;
    }
    return f;
}

// Strict decimal: digits only, no sign, no leading zero, at most `max`.
bool parseNumber(std::string_view s, std::uint64_t max, std::uint64_t &out) {
    if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + std::uint64_t(c - '0');
    }
    if (v > max) {
        return false;
    }
    out = v;
    return true;
}

ProtoError checkFrame(std::string_view frame) {
    if (frame.size() > kMaxFrameBytes) {
        return ProtoError::TooLong;
    }
    for (const char c : frame) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && u != '\t') || u == 0x7F) {
            return ProtoError::BadChars;
        }
    }
    return ProtoError::None;
}

bool bubbleNumber(std::string_view s, std::uint32_t &out) {
    std::uint64_t v = 0;
    if (!parseNumber(s, kMaxBubble, v) || v == 0) {
        return false;
    }
    out = std::uint32_t(v);
    return true;
}

struct CodeName {
    ErrorCode code;
    const char *text;
};
constexpr CodeName kCodes[] = {
    {ErrorCode::NameTaken, "name-taken"},
    {ErrorCode::BadName, "bad-name"},
    {ErrorCode::TooLong, "too-long"},
    {ErrorCode::Rate, "rate"},
    {ErrorCode::Full, "full"},
    {ErrorCode::Banned, "banned"},
    {ErrorCode::BadLine, "bad-line"},
};

struct KindName {
    ServerLine::Kind kind;
    const char *text;
    std::size_t fields;
};
constexpr KindName kKinds[] = {
    {ServerLine::Kind::Welcome, "welcome", 2},
    {ServerLine::Kind::Users, "users", 2},
    {ServerLine::Kind::Hist, "hist", 5},
    {ServerLine::Kind::Ready, "ready", 1},
    {ServerLine::Kind::Draft, "draft", 3},
    {ServerLine::Kind::Gone, "gone", 2},
    {ServerLine::Kind::Typers, "typers", 2},
    {ServerLine::Kind::Msg, "msg", 4},
    {ServerLine::Kind::Ack, "ack", 4},
    {ServerLine::Kind::Online, "online", 2},
    {ServerLine::Kind::Join, "join", 2},
    {ServerLine::Kind::Leave, "leave", 2},
    {ServerLine::Kind::Hide, "hide", 2},
    {ServerLine::Kind::Notice, "notice", 2},
    {ServerLine::Kind::Error, "error", 3},
};

} // namespace

const char *errorCodeText(ErrorCode code) {
    for (const auto &c : kCodes) {
        if (c.code == code) {
            return c.text;
        }
    }
    return "bad-line";
}

ClientParse parseClientLine(std::string_view frame) {
    ClientParse result;
    if (const auto e = checkFrame(frame); e != ProtoError::None) {
        result.error = e;
        return result;
    }
    const Fields f = split(frame);
    const auto cmd = f.part[0];
    auto fail = [&](ProtoError e) {
        result.error = e;
        return result;
    };
    if (cmd == "hello" || cmd == "nick") {
        if (f.tooMany || f.count != 2) {
            return fail(ProtoError::Fields);
        }
        if (checkName(f.part[1]) != NameError::None) {
            return fail(ProtoError::Name);
        }
        result.line.kind =
            cmd == "hello" ? ClientLine::Kind::Hello : ClientLine::Kind::Nick;
        result.line.name = std::string(f.part[1]);
    }
    else if (cmd == "draft") {
        if (f.tooMany || f.count != 3) {
            return fail(ProtoError::Fields);
        }
        if (!bubbleNumber(f.part[1], result.line.bubble)) {
            return fail(ProtoError::Number);
        }
        if (!checkDraft(f.part[2]).ok()) {
            return fail(ProtoError::Text);
        }
        result.line.kind = ClientLine::Kind::Draft;
        result.line.text = std::string(f.part[2]);
    }
    else if (cmd == "lock") {
        if (f.tooMany || f.count != 2) {
            return fail(ProtoError::Fields);
        }
        if (!bubbleNumber(f.part[1], result.line.bubble)) {
            return fail(ProtoError::Number);
        }
        result.line.kind = ClientLine::Kind::Lock;
    }
    else {
        return fail(ProtoError::Unknown);
    }
    return result;
}

std::string formatClientLine(const ClientLine &line) {
    switch (line.kind) {
    case ClientLine::Kind::Hello:
        return "hello\t" + line.name;
    case ClientLine::Kind::Nick:
        return "nick\t" + line.name;
    case ClientLine::Kind::Draft:
        return "draft\t" + std::to_string(line.bubble) + "\t" + line.text;
    case ClientLine::Kind::Lock:
        return "lock\t" + std::to_string(line.bubble);
    }
    return {};
}

ServerParse parseServerLine(std::string_view frame) {
    ServerParse result;
    auto fail = [&](ProtoError e) {
        result.error = e;
        result.line = ServerLine{};
        return result;
    };
    if (const auto e = checkFrame(frame); e != ProtoError::None) {
        return fail(e);
    }
    const Fields f = split(frame);
    const KindName *kind = nullptr;
    for (const auto &k : kKinds) {
        if (f.part[0] == k.text) {
            kind = &k;
        }
    }
    if (!kind) {
        return fail(ProtoError::Unknown);
    }
    if (f.tooMany || f.count != kind->fields) {
        return fail(ProtoError::Fields);
    }
    ServerLine &l = result.line;
    l.kind = kind->kind;
    auto name = [&](std::string_view s) {
        if (checkName(s) != NameError::None) {
            return false;
        }
        l.name = std::string(s);
        return true;
    };
    auto number = [&](std::string_view s, std::uint64_t max, auto &target) {
        std::uint64_t v = 0;
        if (!parseNumber(s, max, v)) {
            return false;
        }
        target = static_cast<std::decay_t<decltype(target)>>(v);
        return true;
    };
    constexpr std::uint64_t kMax32 = 0xFFFFFFFFull;
    constexpr std::uint64_t kMaxSerial = 999999999999999999ull;
    switch (l.kind) {
    case ServerLine::Kind::Welcome:
    case ServerLine::Kind::Gone:
    case ServerLine::Kind::Join:
    case ServerLine::Kind::Leave:
        if (!name(f.part[1])) {
            return fail(ProtoError::Name);
        }
        break;
    case ServerLine::Kind::Users: {
        const auto list = f.part[1];
        std::size_t start = 0;
        while (!list.empty() && start <= list.size()) {
            const auto comma = list.find(',', start);
            const auto one = list.substr(
                start, comma == std::string_view::npos ? comma : comma - start);
            if (checkName(one) != NameError::None) {
                return fail(ProtoError::Name);
            }
            l.names.emplace_back(one);
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        break;
    }
    case ServerLine::Kind::Hist:
        if (!number(f.part[1], kMaxSerial, l.serial) ||
            !number(f.part[2], kMax32, l.age)) {
            return fail(ProtoError::Number);
        }
        if (!name(f.part[3])) {
            return fail(ProtoError::Name);
        }
        l.text = sanitizeIncoming(f.part[4]);
        break;
    case ServerLine::Kind::Ready:
        break;
    case ServerLine::Kind::Draft:
        if (!name(f.part[1])) {
            return fail(ProtoError::Name);
        }
        l.text = sanitizeIncoming(f.part[2]);
        break;
    case ServerLine::Kind::Typers:
    case ServerLine::Kind::Online:
        if (!number(f.part[1], kMax32, l.count)) {
            return fail(ProtoError::Number);
        }
        break;
    case ServerLine::Kind::Msg:
        if (!number(f.part[1], kMaxSerial, l.serial)) {
            return fail(ProtoError::Number);
        }
        if (!name(f.part[2])) {
            return fail(ProtoError::Name);
        }
        l.text = sanitizeIncoming(f.part[3]);
        break;
    case ServerLine::Kind::Ack:
        if (!number(f.part[1], kMaxBubble, l.bubble) || l.bubble == 0 ||
            !number(f.part[2], kMaxSerial, l.serial)) {
            return fail(ProtoError::Number);
        }
        l.text = sanitizeIncoming(f.part[3]);
        break;
    case ServerLine::Kind::Hide:
        if (!number(f.part[1], kMaxSerial, l.serial)) {
            return fail(ProtoError::Number);
        }
        break;
    case ServerLine::Kind::Notice:
        l.text = sanitizeIncoming(f.part[1]);
        break;
    case ServerLine::Kind::Error: {
        bool known = false;
        for (const auto &c : kCodes) {
            if (f.part[1] == c.text) {
                l.code = c.code;
                known = true;
            }
        }
        if (!known) {
            return fail(ProtoError::Unknown);
        }
        l.text = sanitizeIncoming(f.part[2]);
        break;
    }
    }
    return result;
}

std::string formatServerLine(const ServerLine &l) {
    const char *cmd = "ready";
    for (const auto &k : kKinds) {
        if (k.kind == l.kind) {
            cmd = k.text;
        }
    }
    std::string out = cmd;
    auto add = [&](const std::string &s) { out += '\t' + s; };
    switch (l.kind) {
    case ServerLine::Kind::Welcome:
    case ServerLine::Kind::Gone:
    case ServerLine::Kind::Join:
    case ServerLine::Kind::Leave:
        add(l.name);
        break;
    case ServerLine::Kind::Users: {
        std::string list;
        for (const auto &n : l.names) {
            list += (list.empty() ? "" : ",") + n;
        }
        add(list);
        break;
    }
    case ServerLine::Kind::Hist:
        add(std::to_string(l.serial));
        add(std::to_string(l.age));
        add(l.name);
        add(l.text);
        break;
    case ServerLine::Kind::Ready:
        break;
    case ServerLine::Kind::Draft:
        add(l.name);
        add(l.text);
        break;
    case ServerLine::Kind::Typers:
    case ServerLine::Kind::Online:
        add(std::to_string(l.count));
        break;
    case ServerLine::Kind::Msg:
        add(std::to_string(l.serial));
        add(l.name);
        add(l.text);
        break;
    case ServerLine::Kind::Ack:
        add(std::to_string(l.bubble));
        add(std::to_string(l.serial));
        add(l.text);
        break;
    case ServerLine::Kind::Hide:
        add(std::to_string(l.serial));
        break;
    case ServerLine::Kind::Notice:
        add(l.text);
        break;
    case ServerLine::Kind::Error:
        add(errorCodeText(l.code));
        add(l.text);
        break;
    }
    return out;
}

} // namespace chat
