#include "config.h"
#include "message.h"
#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fast-json/json.h>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace server {
namespace {
using json::JsonNode;
using json::TokenType;
constexpr std::size_t maxFileSize = 64 * 1024;

void appendUtf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 63));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    }
    else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 63));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    }
}
std::optional<unsigned> hex4(std::string_view text) {
    if (text.size() < 4)
        return {};
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return {};
        value = value * 16 + digit;
    }
    return value;
}
// Decodes the text between the quotes of a JSON string, \u escapes included.
// Returns false for a bad escape or an unpaired surrogate.
bool unescape(std::string_view raw, std::string &out) {
    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (static_cast<unsigned char>(c) < 32)
            return false;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i == raw.size())
            return false;
        switch (raw[i]) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'u': {
            auto cp = hex4(raw.substr(i + 1));
            if (!cp)
                return false;
            i += 4;
            if (*cp >= 0xd800 && *cp <= 0xdbff) {
                if (raw.substr(i + 1, 2) != "\\u")
                    return false;
                auto low = hex4(raw.substr(i + 3));
                if (!low || *low < 0xdc00 || *low > 0xdfff)
                    return false;
                i += 6;
                cp = 0x10000 + ((*cp - 0xd800) << 10) + (*low - 0xdc00);
            }
            else if (*cp >= 0xdc00 && *cp <= 0xdfff) {
                return false;
            }
            appendUtf8(out, *cp);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

struct Context {
    std::string_view text;
    const std::string &label;
};
// fast-json validates document structure, but leaves string contents raw.
// Validate every string, including ignored comment subtrees, before committing
// the settings. Iteration avoids recursion on deeply nested untrusted input.
void validateStrings(const Context &context, const JsonNode &root) {
    std::vector<const JsonNode *> pending{&root};
    std::string decoded;
    while (!pending.empty()) {
        const auto *node = pending.back();
        pending.pop_back();
        if (node->next())
            pending.push_back(node->next());
        if (node->children())
            pending.push_back(node->children());
        const auto &token = node->value();
        if (token.type != TokenType::STRING && token.type != TokenType::KEY)
            continue;
        if (!unescape(token.value, decoded) || !protocol::validUtf8(decoded) ||
            decoded.find('\0') != std::string::npos) {
            const auto at = json::get_token_position(token, context.text);
            throw ConfigError(context.label + ":" +
                              std::to_string(at.line_number) + ":" +
                              std::to_string(at.column_number - 1) +
                              ": syntax error: invalid JSON string");
        }
    }
}
// One member (or array element) being read; `key` provides the position.
struct Member {
    const Context &context;
    std::string path;
    const JsonNode &key;
    const JsonNode &value;
    [[noreturn]] void fail(const std::string &what) const {
        const auto at = json::get_token_position(key.value(), context.text);
        // Tokens exclude their opening quote; point at it.
        throw ConfigError(context.label + ":" + std::to_string(at.line_number) +
                          ":" + std::to_string(at.column_number - 1) + ": " +
                          path + ": " + what);
    }
    TokenType type() const {
        return value.value().type;
    }
};
using Apply = std::function<void(const Member &)>;
struct Field {
    std::string name;
    Apply apply;
};

std::string decodedKey(const Context &context,
                       const std::string &path,
                       const JsonNode &key) {
    std::string name;
    if (!unescape(key.value().value, name)) {
        const Member member{context, path, key, key};
        member.fail("invalid escape sequence in the key");
    }
    return name;
}
void readObject(const Context &context,
                const JsonNode &object,
                const std::string &path,
                const std::vector<Field> &fields) {
    std::set<std::string> seen;
    for (const auto &key : object) {
        const auto name = decodedKey(context, path, key);
        if (!name.empty() && name[0] == '_')
            continue; // Comment.
        Member member{context,
                      path.empty() ? name : path + "." + name,
                      key,
                      *key.children()};
        if (!seen.insert(name).second)
            member.fail("duplicate key");
        const auto field =
            std::find_if(fields.begin(), fields.end(), [&](const Field &f) {
                return f.name == name;
            });
        if (field == fields.end())
            member.fail("unknown key");
        field->apply(member);
    }
}
long long integerOf(const Member &member, long long min, long long max) {
    if (member.type() != TokenType::NUMBER)
        member.fail("must be an integer");
    const auto text = member.value.value().value;
    long long value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value < min || value > max)
        member.fail("must be an integer between " + std::to_string(min) +
                    " and " + std::to_string(max));
    return value;
}
template <class T>
Apply integer(T &target, long long min, long long max) {
    return [&target, min, max](const Member &m) {
        target = static_cast<T>(integerOf(m, min, max));
    };
}
template <class D>
Apply duration(D &target, long long min, long long max) {
    return [&target, min, max](const Member &m) {
        target = D(integerOf(m, min, max));
    };
}
Apply boolean(bool &target) {
    return [&target](const Member &m) {
        if (m.type() != TokenType::BOOLEAN)
            m.fail("must be true or false");
        target = m.value.boolean();
    };
}
std::string stringOf(const Member &member, std::size_t maxSize) {
    if (member.type() != TokenType::STRING)
        member.fail("must be a string");
    std::string text;
    if (!unescape(member.value.value().value, text))
        member.fail("invalid escape sequence in the string");
    if (text.empty() || text.size() > maxSize ||
        text.find('\0') != std::string::npos)
        member.fail("must be a non-empty string of at most " +
                    std::to_string(maxSize) + " bytes without NUL");
    return text;
}
Apply text(std::string &target) {
    return [&target](const Member &m) { target = stringOf(m, 4096); };
}
bool validEnvironmentName(const std::string &name) {
    if (name.empty() || name.size() > 255 ||
        std::isdigit(static_cast<unsigned char>(name[0])))
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return c == '_' || std::isalnum(static_cast<unsigned char>(c));
    });
}
Apply stringList(std::vector<std::string> &target,
                 std::size_t minCount,
                 std::size_t maxCount,
                 bool environmentNames) {
    return [&target, minCount, maxCount, environmentNames](const Member &m) {
        if (m.type() != TokenType::BEGIN_ARRAY)
            m.fail("must be an array of strings");
        std::vector<std::string> items;
        for (const auto &element : m.value) {
            Member item{m.context,
                        m.path + "[" + std::to_string(items.size()) + "]",
                        element,
                        element};
            if (items.size() == maxCount)
                m.fail("must have at most " + std::to_string(maxCount) +
                       " entries");
            items.push_back(stringOf(item, 4096));
            if (environmentNames && !validEnvironmentName(items.back()))
                item.fail("must be an environment variable name");
        }
        if (items.size() < minCount)
            m.fail("must have at least " + std::to_string(minCount) +
                   " entries");
        target = std::move(items);
    };
}
Apply section(std::function<std::vector<Field>()> make) {
    return [make](const Member &m) {
        if (m.type() != TokenType::BEGIN_OBJECT)
            m.fail("must be an object");
        readObject(m.context, m.value, m.path, make());
    };
}
// `--print-config` shows these in place of the hash; the hash itself lives in
// the credentials file and is never accepted here.
constexpr const char *hashSet = "<redacted>";
constexpr const char *hashUnset = "<none>";
Apply passwordHashPlaceholder() {
    return [](const Member &m) {
        std::string value;
        if (m.type() != TokenType::STRING ||
            !unescape(m.value.value().value, value) ||
            (value != hashSet && value != hashUnset))
            m.fail("is output of --print-config only; put the hash in the "
                   "credentials file");
    };
}

std::vector<Field> rateLimitFields(RateLimitConfig &c) {
    return {
        {"max_failures", integer(c.maxFailures, 1, 1000)},
        {"lockout_initial_seconds", duration(c.lockoutInitial, 1, 86400)},
        {"lockout_max_seconds", duration(c.lockoutMax, 1, 2592000)},
        {"global_attempts_per_hour",
         integer(c.globalAttemptsPerHour, 0, 1000000)},
        {"global_counts_failures_only", boolean(c.globalCountsFailuresOnly)},
        {"exempt_loopback", boolean(c.exemptLoopback)},
        {"max_sources", integer(c.maxSources, 1, 1000000)},
        {"source_idle_expiry_seconds",
         duration(c.sourceIdleExpiry, 1, 2592000)},
    };
}
std::vector<Field> authFields(AuthConfig &c) {
    return {
        {"idle_timeout_seconds", duration(c.idleTimeout, 1, 31536000)},
        {"absolute_timeout_seconds", duration(c.absoluteTimeout, 1, 31536000)},
    };
}
std::vector<Field> processFields(ProcessConfig &c) {
    return {
        {"arguments", stringList(c.arguments, 1, 64, false)},
        {"environment_allow_list",
         stringList(c.environmentAllowList, 0, 256, true)},
        {"termination_grace_milliseconds",
         duration(c.terminationGrace, 0, 60000)},
        {"termination_drain_timeout_milliseconds",
         duration(c.terminationDrainTimeout, 0, 60000)},
    };
}
std::vector<Field> timeoutFields(ServerConfig &c) {
    return {
        {"request_milliseconds", duration(c.requestTimeout, 100, 600000)},
        {"websocket_handshake_milliseconds",
         duration(c.webSocketHandshakeTimeout, 100, 600000)},
        {"websocket_idle_milliseconds",
         duration(c.webSocketIdleTimeout, 1000, 86400000)},
    };
}
std::vector<Field> sessionFields(protocol::SessionConfig &c) {
    return {
        {"hello_timeout_milliseconds", duration(c.helloTimeout, 100, 600000)},
        {"idle_timeout_milliseconds", duration(c.idleTimeout, 1000, 86400000)},
        {"ack_stall_timeout_milliseconds",
         duration(c.ackStallTimeout, 1000, 86400000)},
        {"output_window_bytes", integer(c.outputWindow, 32768, 67108864)},
        {"resize_interval_milliseconds", duration(c.resizeInterval, 0, 60000)},
        {"resize_hard_limit", integer(c.resizeHardLimit, 1, 1000000)},
        {"rate_window_milliseconds", duration(c.rateWindow, 100, 600000)},
        {"max_messages_per_window",
         integer(c.maxMessagesPerWindow, 1, 10000000)},
    };
}
std::vector<Field> topLevelFields(Settings &s) {
    auto &c = s.server;
    return {
        {"listen_address", text(c.address)},
        {"port", integer(c.port, 0, 65535)},
        {"allowed_origin", text(c.allowedOrigin)},
        {"allow_plain_http", boolean(c.allowPlainHttp)},
        {"credentials",
         [&s](const Member &m) {
             s.credentialsPath = stringOf(m, 4096);
             s.credentialsDefaulted = false;
         }},
        {"password_hash", passwordHashPlaceholder()},
        {"static_dir", text(c.staticDir)},
        {"max_connections", integer(c.maxConnections, 1, 65535)},
        {"max_unauthenticated", integer(c.maxUnauthenticated, 1, 65535)},
        {"rate_limit", section([&c] { return rateLimitFields(c.rateLimit); })},
        {"auth", section([&c] { return authFields(c.auth); })},
        {"process", section([&c] { return processFields(c.process); })},
        {"timeouts", section([&c] { return timeoutFields(c); })},
        {"session", section([&c] { return sessionFields(c.session); })},
    };
}

bool validOrigin(const std::string &origin) {
    const auto scheme = origin.find("://");
    return (origin.rfind("http://", 0) == 0 ||
            origin.rfind("https://", 0) == 0) &&
           scheme != std::string::npos && origin.size() > scheme + 3 &&
           origin.find_first_of("/\r\n\t @?#", scheme + 3) == std::string::npos;
}

// Small writer for --print-config: fixed key order, 2-space indent, and unlike
// fast-json's JsonOut it can write an empty array.
class Writer {
public:
    void begin(std::string_view key, char open) {
        item(key);
        out_ += open;
        empty_.push_back(true);
    }
    void end(char close) {
        const bool wasEmpty = empty_.back();
        empty_.pop_back();
        if (!wasEmpty) {
            out_ += '\n';
            out_.append(empty_.size() * 2, ' ');
        }
        out_ += close;
    }
    void member(std::string_view key, std::string_view value) {
        item(key);
        quote(value);
    }
    void member(std::string_view key, const std::string &value) {
        member(key, std::string_view(value));
    }
    void member(std::string_view key, const char *value) {
        member(key, std::string_view(value));
    }
    void member(std::string_view key, bool value) {
        item(key);
        out_ += value ? "true" : "false";
    }
    void member(std::string_view key, long long value) {
        item(key);
        out_ += std::to_string(value);
    }
    template <class T,
              class = std::enable_if_t<std::is_integral_v<T> &&
                                       !std::is_same_v<T, bool>>>
    void member(std::string_view key, T value) {
        member(key, static_cast<long long>(value));
    }
    std::string finish() {
        return out_ + "\n";
    }

private:
    void item(std::string_view key) {
        if (!empty_.empty()) {
            if (!empty_.back())
                out_ += ',';
            out_ += '\n';
            out_.append(empty_.size() * 2, ' ');
            empty_.back() = false;
        }
        if (!key.empty()) {
            quote(key);
            out_ += ": ";
        }
    }
    void quote(std::string_view text) {
        out_ += '"';
        for (const char c : text) {
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char digits[] = "0123456789abcdef";
                    out_ += "\\u00";
                    out_ += digits[(c >> 4) & 15];
                    out_ += digits[c & 15];
                }
                else {
                    out_ += c;
                }
            }
        }
        out_ += '"';
    }
    std::string out_;
    std::vector<bool> empty_;
};
void element(Writer &w, const std::string &value) {
    w.member({}, value);
}
} // namespace

std::string defaultConfigDirectory(const GetEnv &getenv) {
    const auto absolute = [&](const char *name) -> const char * {
        const char *value = getenv(name);
        return value && value[0] == '/' ? value : nullptr;
    };
    if (const char *xdg = absolute("XDG_CONFIG_HOME"))
        return std::string(xdg) + "/matterm";
    if (const char *home = absolute("HOME"))
        return std::string(home) + "/.config/matterm";
    return {};
}

void validateSettings(const Settings &settings) {
    const auto &c = settings.server;
    boost::system::error_code ec;
    boost::asio::ip::make_address(c.address, ec);
    if (ec)
        throw ConfigError("listen_address: must be an IPv4 or IPv6 address");
    if (!validOrigin(c.allowedOrigin))
        throw ConfigError("allowed_origin: must be http:// or https:// "
                          "followed by host[:port], without a path");
    if (c.rateLimit.lockoutMax < c.rateLimit.lockoutInitial)
        throw ConfigError("rate_limit.lockout_max_seconds: must not be less "
                          "than rate_limit.lockout_initial_seconds");
    if (c.process.arguments.empty())
        throw ConfigError("process.arguments: must name a program");
}

void applyConfigText(std::string_view text,
                     Settings &settings,
                     const std::string &label) {
    if (text.size() > maxFileSize)
        throw ConfigError(label + ": too large (limit " +
                          std::to_string(maxFileSize) + " bytes)");
    Settings work = settings;
    try {
        json::JsonRoot root;
        try {
            root = json::parse_json(text);
        }
        catch (const json::ParseError &error) {
            throw ConfigError(label + ":" + std::to_string(error.line()) + ":" +
                              std::to_string(error.column()) +
                              ": syntax error: " + error.message());
        }
        if (root->value().type != TokenType::BEGIN_OBJECT) {
            const auto start = text.find_first_not_of(" \t\r\n");
            const auto at = json::get_offset_position(text, start);
            throw ConfigError(label + ":" + std::to_string(at.line_number) +
                              ":" + std::to_string(at.column_number) +
                              ": syntax error: the document must be an object");
        }
        const Context context{text, label};
        readObject(context, *root, {}, topLevelFields(work));
        // Preserve schema diagnostics (key paths) for known fields; work is
        // still a private copy, so a later string failure changes nothing.
        validateStrings(context, *root);
    }
    catch (const ConfigError &) {
        throw;
    }
    catch (const std::exception &error) {
        throw ConfigError(label +
                          ": cannot read the document: " + error.what());
    }
    try {
        validateSettings(work);
    }
    catch (const ConfigError &error) {
        throw ConfigError(label + ": " + error.what());
    }
    settings = std::move(work);
}

bool loadConfigFile(const std::string &path,
                    ConfigFile mode,
                    Settings &settings) {
    const int fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT && mode == ConfigFile::Optional)
            return false;
        if (errno == ELOOP)
            throw ConfigError(path + ": refusing to follow a symbolic link");
        throw ConfigError(path + ": cannot open: " + std::strerror(errno));
    }
    struct Closer {
        int fd;
        ~Closer() {
            ::close(fd);
        }
    } closer{fd};
    struct stat st {};
    if (::fstat(fd, &st) != 0)
        throw ConfigError(path + ": cannot inspect: " + std::strerror(errno));
    if (!S_ISREG(st.st_mode))
        throw ConfigError(path + ": not a regular file");
    if (st.st_uid != ::geteuid())
        throw ConfigError(path + ": not owned by the current user");
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        throw ConfigError(path + ": writable by group or others (chmod go-w)");
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > maxFileSize)
        throw ConfigError(path + ": too large (limit " +
                          std::to_string(maxFileSize) + " bytes)");
    std::string text(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t done = 0;
    while (done < text.size()) {
        const auto count = ::read(fd, text.data() + done, text.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        done += static_cast<std::size_t>(count);
    }
    text.resize(done);
    applyConfigText(text, settings, path);
    return true;
}

std::string printConfig(const Settings &settings) {
    const auto &c = settings.server;
    Writer w;
    w.begin({}, '{');
    w.member("listen_address", c.address);
    w.member("port", c.port);
    w.member("allowed_origin", c.allowedOrigin);
    w.member("allow_plain_http", c.allowPlainHttp);
    if (!settings.credentialsPath.empty())
        w.member("credentials", settings.credentialsPath);
    w.member("password_hash", c.passwordHash.empty() ? hashUnset : hashSet);
    if (!c.staticDir.empty())
        w.member("static_dir", c.staticDir);
    w.member("max_connections", c.maxConnections);
    w.member("max_unauthenticated", c.maxUnauthenticated);
    w.begin("rate_limit", '{');
    w.member("max_failures", c.rateLimit.maxFailures);
    w.member("lockout_initial_seconds", c.rateLimit.lockoutInitial.count());
    w.member("lockout_max_seconds", c.rateLimit.lockoutMax.count());
    w.member("global_attempts_per_hour", c.rateLimit.globalAttemptsPerHour);
    w.member("global_counts_failures_only",
             c.rateLimit.globalCountsFailuresOnly);
    w.member("exempt_loopback", c.rateLimit.exemptLoopback);
    w.member("max_sources", c.rateLimit.maxSources);
    w.member("source_idle_expiry_seconds",
             c.rateLimit.sourceIdleExpiry.count());
    w.end('}');
    w.begin("auth", '{');
    w.member("idle_timeout_seconds", c.auth.idleTimeout.count());
    w.member("absolute_timeout_seconds", c.auth.absoluteTimeout.count());
    w.end('}');
    w.begin("process", '{');
    w.begin("arguments", '[');
    for (const auto &argument : c.process.arguments)
        element(w, argument);
    w.end(']');
    w.begin("environment_allow_list", '[');
    for (const auto &name : c.process.environmentAllowList)
        element(w, name);
    w.end(']');
    w.member("termination_grace_milliseconds",
             c.process.terminationGrace.count());
    w.member("termination_drain_timeout_milliseconds",
             c.process.terminationDrainTimeout.count());
    w.end('}');
    w.begin("timeouts", '{');
    w.member("request_milliseconds", c.requestTimeout.count());
    w.member("websocket_handshake_milliseconds",
             c.webSocketHandshakeTimeout.count());
    w.member("websocket_idle_milliseconds", c.webSocketIdleTimeout.count());
    w.end('}');
    w.begin("session", '{');
    w.member("hello_timeout_milliseconds", c.session.helloTimeout.count());
    w.member("idle_timeout_milliseconds", c.session.idleTimeout.count());
    w.member("ack_stall_timeout_milliseconds",
             c.session.ackStallTimeout.count());
    w.member("output_window_bytes", c.session.outputWindow);
    w.member("resize_interval_milliseconds", c.session.resizeInterval.count());
    w.member("resize_hard_limit", c.session.resizeHardLimit);
    w.member("rate_window_milliseconds", c.session.rateWindow.count());
    w.member("max_messages_per_window", c.session.maxMessagesPerWindow);
    w.end('}');
    w.end('}');
    return w.finish();
}

CommandLine parseCommandLine(const std::vector<std::string> &arguments) {
    CommandLine result;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto &option = arguments[i];
        const auto value = [&]() -> const std::string & {
            if (++i == arguments.size())
                throw ConfigError(option + ": missing value");
            return arguments[i];
        };
        if (option == "--config") {
            result.configPath = value();
        }
        else if (option == "--listen") {
            result.listen = value();
        }
        else if (option == "--port") {
            const auto &text = value();
            unsigned number = 0;
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size() || number > 65535)
                throw ConfigError("--port: must be an integer between 0 and "
                                  "65535");
            result.port = static_cast<unsigned short>(number);
        }
        else if (option == "--origin") {
            result.origin = value();
        }
        else if (option == "--credentials") {
            result.credentials = value();
        }
        else if (option == "--static-dir") {
            result.staticDir = value();
        }
        else if (option == "--allow-plain-http") {
            result.allowPlainHttp = true;
        }
        else if (option == "--set-password" ||
                 option == "--setup-credentials") {
            result.setPassword = true;
        }
        else if (option == "--print-config") {
            result.printConfig = true;
        }
        else if (option == "--help") {
            result.help = true;
        }
        else if (option == "--") {
            result.program = std::vector<std::string>(arguments.begin() + i + 1,
                                                      arguments.end());
            if (result.program->empty())
                throw ConfigError("--: missing program");
            break;
        }
        else {
            throw ConfigError("unknown option " + option);
        }
    }
    return result;
}

void applyCommandLine(const CommandLine &commandLine, Settings &settings) {
    auto &c = settings.server;
    if (commandLine.listen)
        c.address = *commandLine.listen;
    if (commandLine.port)
        c.port = *commandLine.port;
    if (commandLine.origin)
        c.allowedOrigin = *commandLine.origin;
    if (commandLine.credentials) {
        settings.credentialsPath = *commandLine.credentials;
        settings.credentialsDefaulted = false;
    }
    if (commandLine.staticDir)
        c.staticDir = *commandLine.staticDir;
    if (commandLine.allowPlainHttp)
        c.allowPlainHttp = true;
    if (commandLine.program)
        c.process.arguments = *commandLine.program;
}

Settings resolveSettings(const CommandLine &commandLine, const GetEnv &getenv) {
    Settings settings;
    std::string configPath;
    if (commandLine.configPath) {
        configPath = *commandLine.configPath;
    }
    else if (const auto directory = defaultConfigDirectory(getenv);
             !directory.empty()) {
        configPath = directory + "/server.json";
    }
    if (!configPath.empty())
        loadConfigFile(configPath,
                       commandLine.configPath ? ConfigFile::Required
                                              : ConfigFile::Optional,
                       settings);
    applyCommandLine(commandLine, settings);
    if (settings.credentialsPath.empty() && !configPath.empty()) {
        auto directory = std::filesystem::path(configPath).parent_path();
        if (directory.empty())
            directory = ".";
        settings.credentialsPath = (directory / "credentials").string();
        settings.credentialsDefaulted = true;
    }
    validateSettings(settings);
    return settings;
}
} // namespace server
