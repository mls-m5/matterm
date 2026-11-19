#include "config.h"
#include "mls-unit-test/unittest.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace server;
using namespace std::chrono;
namespace {
// The message of the ConfigError that applying `text` throws, or "" for none.
std::string errorOf(const std::string &text) {
    Settings settings;
    try {
        applyConfigText(text, settings, "t.json");
    }
    catch (const ConfigError &error) {
        return error.what();
    }
    return {};
}
bool contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}
struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/matterm-config-XXXXXX";
        path = ::mkdtemp(pattern) ? pattern : "";
    }
    ~TempDir() {
        std::string command = "rm -rf '" + path + "'";
        if (!path.empty() && ::system(command.c_str()) != 0) {
        }
    }
    std::string write(const std::string &name,
                      const std::string &content,
                      mode_t mode = 0600) const {
        const auto file = path + "/" + name;
        std::ofstream(file) << content;
        ::chmod(file.c_str(), mode);
        return file;
    }
};
std::string slurp(const std::string &file) {
    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}
const char *noEnvironment(const char *) {
    return nullptr;
}
} // namespace
TEST_SUIT_BEGIN(ServerConfig)
TEST_CASE("An empty document leaves today's defaults") {
    Settings settings;
    applyConfigText("{}", settings);
    const ServerConfig &c = settings.server;
    EXPECT_EQ(c.address, "127.0.0.1");
    EXPECT_EQ(c.port, 8080);
    EXPECT_EQ(c.allowedOrigin, "http://localhost:8080");
    EXPECT(!c.allowPlainHttp);
    EXPECT_EQ(c.maxConnections, 32u);
    EXPECT_EQ(c.maxUnauthenticated, 16u);
    EXPECT_EQ(c.rateLimit.maxFailures, 5u);
    EXPECT_EQ(c.rateLimit.lockoutInitial.count(), 30);
    EXPECT_EQ(c.rateLimit.lockoutMax.count(), 900);
    EXPECT_EQ(c.rateLimit.globalAttemptsPerHour, 30u);
    EXPECT(c.rateLimit.exemptLoopback);
    EXPECT_EQ(c.rateLimit.maxSources, std::size_t(4096));
    EXPECT_EQ(c.rateLimit.sourceIdleExpiry.count(), 3600);
    EXPECT(c.rateLimit.globalCountsFailuresOnly);
    EXPECT_EQ(c.auth.idleTimeout.count(), 43200);
    EXPECT_EQ(c.auth.absoluteTimeout.count(), 604800);
    EXPECT_EQ(c.process.arguments.size(), std::size_t(1));
    EXPECT_EQ(c.process.arguments[0], "/bin/sh");
    EXPECT_EQ(c.process.environmentAllowList.size(), std::size_t(7));
    EXPECT_EQ(c.process.terminationGrace.count(), 500);
    EXPECT_EQ(c.process.terminationDrainTimeout.count(), 1000);
    EXPECT_EQ(c.requestTimeout.count(), 5000);
    EXPECT_EQ(c.webSocketHandshakeTimeout.count(), 5000);
    EXPECT_EQ(c.webSocketIdleTimeout.count(), 60000);
    EXPECT_EQ(c.session.helloTimeout.count(), 5000);
    EXPECT_EQ(c.session.idleTimeout.count(), 60000);
    EXPECT_EQ(c.session.ackStallTimeout.count(), 30000);
    EXPECT_EQ(c.session.outputWindow, std::size_t(262144));
    EXPECT(c.staticDir.empty());
    EXPECT(settings.credentialsPath.empty());
}
TEST_CASE("Every section loads") {
    Settings settings;
    applyConfigText(R"({
      "listen_address": "::1", "port": 0,
      "allowed_origin": "https://term.example.org", "allow_plain_http": true,
      "credentials": "/etc/matterm/credentials", "static_dir": "/srv/web",
      "max_connections": 4, "max_unauthenticated": 2,
      "rate_limit": {"max_failures": 3, "lockout_initial_seconds": 60,
        "lockout_max_seconds": 120, "global_attempts_per_hour": 0,
        "global_counts_failures_only": false, "exempt_loopback": false,
        "max_sources": 10, "source_idle_expiry_seconds": 90},
      "auth": {"idle_timeout_seconds": 100, "absolute_timeout_seconds": 200},
      "process": {"arguments": ["/bin/bash", "-l"],
        "environment_allow_list": ["HOME", "_X1"],
        "termination_grace_milliseconds": 0,
        "termination_drain_timeout_milliseconds": 2000},
      "timeouts": {"request_milliseconds": 1000,
        "websocket_handshake_milliseconds": 2000,
        "websocket_idle_milliseconds": 3000},
      "session": {"hello_timeout_milliseconds": 1100,
        "idle_timeout_milliseconds": 2200, "ack_stall_timeout_milliseconds": 3300,
        "output_window_bytes": 65536, "resize_interval_milliseconds": 50,
        "resize_hard_limit": 7, "rate_window_milliseconds": 900,
        "max_messages_per_window": 99}
    })",
                    settings);
    const ServerConfig &c = settings.server;
    EXPECT_EQ(c.address, "::1");
    EXPECT_EQ(c.port, 0);
    EXPECT_EQ(c.allowedOrigin, "https://term.example.org");
    EXPECT(c.allowPlainHttp);
    EXPECT_EQ(settings.credentialsPath, "/etc/matterm/credentials");
    EXPECT_EQ(c.staticDir, "/srv/web");
    EXPECT_EQ(c.maxConnections, 4u);
    EXPECT_EQ(c.maxUnauthenticated, 2u);
    EXPECT_EQ(c.rateLimit.maxFailures, 3u);
    EXPECT_EQ(c.rateLimit.lockoutInitial.count(), 60);
    EXPECT_EQ(c.rateLimit.lockoutMax.count(), 120);
    EXPECT_EQ(c.rateLimit.globalAttemptsPerHour, 0u);
    EXPECT(!c.rateLimit.globalCountsFailuresOnly);
    EXPECT(!c.rateLimit.exemptLoopback);
    EXPECT_EQ(c.rateLimit.maxSources, std::size_t(10));
    EXPECT_EQ(c.rateLimit.sourceIdleExpiry.count(), 90);
    EXPECT_EQ(c.auth.idleTimeout.count(), 100);
    EXPECT_EQ(c.auth.absoluteTimeout.count(), 200);
    EXPECT_EQ(c.process.arguments.size(), std::size_t(2));
    EXPECT_EQ(c.process.arguments[1], "-l");
    EXPECT_EQ(c.process.environmentAllowList.size(), std::size_t(2));
    EXPECT_EQ(c.process.terminationGrace.count(), 0);
    EXPECT_EQ(c.process.terminationDrainTimeout.count(), 2000);
    EXPECT_EQ(c.requestTimeout.count(), 1000);
    EXPECT_EQ(c.webSocketHandshakeTimeout.count(), 2000);
    EXPECT_EQ(c.webSocketIdleTimeout.count(), 3000);
    EXPECT_EQ(c.session.helloTimeout.count(), 1100);
    EXPECT_EQ(c.session.idleTimeout.count(), 2200);
    EXPECT_EQ(c.session.ackStallTimeout.count(), 3300);
    EXPECT_EQ(c.session.outputWindow, std::size_t(65536));
    EXPECT_EQ(c.session.resizeInterval.count(), 50);
    EXPECT_EQ(c.session.resizeHardLimit, 7u);
    EXPECT_EQ(c.session.rateWindow.count(), 900);
    EXPECT_EQ(c.session.maxMessagesPerWindow, 99u);
    // An empty allow list is meaningful: the child gets no inherited variables.
    applyConfigText(R"({"process": {"environment_allow_list": []}})", settings);
    EXPECT(settings.server.process.environmentAllowList.empty());
    // Only what is named changes.
    EXPECT_EQ(settings.server.port, 0);
}
TEST_CASE("Strings decode escapes, including unicode") {
    Settings settings;
    applyConfigText(
        R"({"process": {"arguments": ["/bin/echo", "a\tb\u00e9\ud83d\ude00\"\\"]}})",
        settings);
    EXPECT_EQ(settings.server.process.arguments[1],
              "a\tb\xc3\xa9\xf0\x9f\x98\x80\"\\");
    EXPECT(contains(errorOf(R"({"static_dir": "\ud800"})"), "escape"));
    EXPECT(contains(errorOf(R"({"static_dir": "a\u0000b"})"), "static_dir"));
}
TEST_CASE("Members starting with an underscore are ignored at any depth") {
    Settings settings;
    applyConfigText(R"({
      "_note": "top", "_note": "even twice",
      "_unknown": {"anything": [1, 2, {"x": null}, true, "s"], "port": "no"},
      "port": 9000,
      "rate_limit": {"_why": "because", "max_failures": 7, "_": 1},
      "process": {"_a": [], "arguments": ["/bin/true"]}
    })",
                    settings);
    EXPECT_EQ(settings.server.port, 9000);
    EXPECT_EQ(settings.server.rateLimit.maxFailures, 7u);
    EXPECT_EQ(settings.server.process.arguments[0], "/bin/true");
    // A comment must still be well-formed JSON, and numbers are integers
    // everywhere, comments included.
    EXPECT(contains(errorOf(R"({"_a": [1,]})"), "syntax error"));
    EXPECT(contains(errorOf(R"({"_a": 1.5})"), "only integers"));
    // The underscore marks a comment only at the start of the name.
    EXPECT(contains(errorOf(R"({"a_": 1})"), "a_: unknown key"));
}
TEST_CASE("Unknown keys name their full path") {
    EXPECT(contains(errorOf(R"({"prt": 1})"), "t.json:1:2: prt: unknown key"));
    EXPECT(contains(errorOf(R"({"rate_limit": {"max_failurs": 1}})"),
                    "rate_limit.max_failurs: unknown key"));
    EXPECT(contains(errorOf("{\n  \"session\": {\n    \"x\": 1\n  }\n}"),
                    "t.json:3:5: session.x: unknown key"));
    // Keys the server does not support yet are unknown, not silently accepted.
    EXPECT(contains(errorOf(R"({"trusted_proxy": "127.0.0.1"})"),
                    "trusted_proxy: unknown key"));
}
TEST_CASE("Wrong types name their path") {
    EXPECT(
        contains(errorOf(R"({"port": "8080"})"), "port: must be an integer"));
    EXPECT(contains(errorOf(R"({"port": null})"), "port: must be an integer"));
    EXPECT(contains(errorOf(R"({"port": true})"), "port: must be an integer"));
    EXPECT(contains(errorOf(R"({"allow_plain_http": 1})"),
                    "allow_plain_http: must be true or false"));
    EXPECT(contains(errorOf(R"({"allowed_origin": 5})"),
                    "allowed_origin: must be a string"));
    EXPECT(contains(errorOf(R"({"rate_limit": 5})"),
                    "rate_limit: must be an object"));
    EXPECT(contains(errorOf(R"({"rate_limit": {"exempt_loopback": "yes"}})"),
                    "rate_limit.exempt_loopback: must be true or false"));
    EXPECT(contains(errorOf(R"({"process": {"arguments": "sh"}})"),
                    "process.arguments: must be an array of strings"));
    EXPECT(contains(errorOf(R"({"process": {"arguments": ["sh", 1]}})"),
                    "process.arguments[1]: must be a string"));
    EXPECT(contains(errorOf(R"({"process": {"arguments": ["sh", ""]}})"),
                    "process.arguments[1]: must be a non-empty string"));
    EXPECT(
        contains(errorOf(R"({"process": {"environment_allow_list": ["A-B"]}})"),
                 "process.environment_allow_list[0]: must be an environment"));
    EXPECT(contains(errorOf(R"({"port": 1.5})"), "syntax error"));
}
TEST_CASE("Out-of-range values and cross-field rules name their path") {
    EXPECT(contains(errorOf(R"({"port": 70000})"),
                    "port: must be an integer between 0 and 65535"));
    EXPECT(contains(errorOf(R"({"port": -1})"),
                    "negative numbers are not accepted"));
    EXPECT(contains(errorOf(R"({"_a": -5})"),
                    "negative numbers are not accepted"));
    EXPECT(contains(errorOf(R"({"port": 99999999999999999999})"),
                    "port: must be an integer between"));
    EXPECT(contains(errorOf(R"({"max_connections": 0})"),
                    "max_connections: must be"));
    EXPECT(contains(errorOf(R"({"rate_limit": {"max_failures": 0}})"),
                    "rate_limit.max_failures: must be"));
    EXPECT(contains(errorOf(R"({"session": {"output_window_bytes": 10}})"),
                    "session.output_window_bytes: must be"));
    EXPECT(contains(errorOf(R"({"process": {"arguments": []}})"),
                    "process.arguments: must have at least 1"));
    EXPECT(contains(
        errorOf(
            R"({"rate_limit": {"lockout_initial_seconds": 100, "lockout_max_seconds": 50}})"),
        "rate_limit.lockout_max_seconds: must not be less"));
    EXPECT(contains(errorOf(R"({"listen_address": "localhost"})"),
                    "listen_address: must be an IPv4 or IPv6 address"));
    EXPECT(contains(errorOf(R"({"allowed_origin": "http://x/path"})"),
                    "allowed_origin: must be http:// or https://"));
    EXPECT(contains(errorOf(R"({"allowed_origin": "ftp://x"})"),
                    "allowed_origin: must be http:// or https://"));
    // Boundaries are inclusive.
    EXPECT(errorOf(R"({"port": 65535, "max_connections": 1})").empty());
}
TEST_CASE("Duplicate keys are errors at every level") {
    EXPECT(
        contains(errorOf(R"({"port": 1, "port": 2})"), "port: duplicate key"));
    EXPECT(contains(
        errorOf(R"({"rate_limit": {"max_failures": 1, "max_failures": 2}})"),
        "rate_limit.max_failures: duplicate key"));
    EXPECT(contains(errorOf(R"({"rate_limit": {}, "rate_limit": {}})"),
                    "rate_limit: duplicate key"));
    // Spelled differently, still the same key.
    EXPECT(contains(errorOf(R"({"port": 1, "po\u0072t": 2})"),
                    "port: duplicate key"));
}
TEST_CASE("Malformed JSON is refused with a position") {
    EXPECT(
        contains(errorOf(""), "syntax error: the document must be an object"));
    EXPECT(contains(errorOf("[]"), "the document must be an object"));
    EXPECT(contains(errorOf("{"), "unexpected end"));
    EXPECT(contains(errorOf("{\"port\": 1"),
                    "t.json:1:11: syntax error: expected ',' or '}'"));
    EXPECT(contains(errorOf("{\"port\": 1,}"), "expected a key"));
    EXPECT(contains(errorOf("{\"port\": 1} x"), "unexpected data after"));
    EXPECT(contains(errorOf("{\"port\": 1}\n{}"),
                    "t.json:2:1: syntax error: unexpected data"));
    EXPECT(contains(errorOf("{\"port\" 1}"), "expected ':'"));
    EXPECT(contains(errorOf("{\"a\": 1 \"b\": 2}"), "expected ',' or '}'"));
    EXPECT(contains(errorOf("{port: 1}"), "expected a key"));
    EXPECT(contains(errorOf("{'port': 1}"), "expected a key"));
    EXPECT(contains(errorOf("{\"port\": 01}"), "leading zeros"));
    EXPECT(contains(errorOf("{\"port\": 1e3}"), "only integers"));
    EXPECT(contains(errorOf("{\"port\": +1}"), "unexpected character"));
    EXPECT(contains(errorOf("{\"port\": tru}"), "unexpected character"));
    EXPECT(contains(errorOf("{\"a\": \"x}"), "unterminated string"));
    EXPECT(contains(errorOf("{\"a\": \"x\ty\"}"), "control character"));
    EXPECT(contains(errorOf("{\"a\": \"\\x\"}"), "invalid escape"));
    EXPECT(
        contains(errorOf("\xef\xbb\xbf{}"), "the document must be an object"));
    EXPECT(contains(errorOf(std::string(70000, ' ') + "{}"), "too large"));
    EXPECT(contains(errorOf(std::string(40, '[') + std::string(40, ']')),
                    "must be an object"));
    EXPECT(contains(
        errorOf("{\"_a\":" + std::string(40, '[') + std::string(40, ']') + "}"),
        "nesting is too deep"));
    // Whitespace of every JSON kind is fine.
    EXPECT(errorOf(" \t\r\n{ \"port\"\n:\t5 }\n").empty());
}
TEST_CASE("Errors never contain values, secrets included") {
    const auto message =
        errorOf(R"({"password_hash": "$argon2id$v=19$SECRETSECRET"})");
    EXPECT(contains(message, "password_hash"));
    EXPECT(!contains(message, "SECRET"));
    const auto origin = errorOf(R"({"allowed_origin": "http://x/SECRETPATH"})");
    EXPECT(!contains(origin, "SECRETPATH"));
    EXPECT(!contains(errorOf(R"({"listen_address": "SECRETHOST"})"),
                     "SECRETHOST"));
    EXPECT(!contains(errorOf(R"({"port": "SECRETPORT"})"), "SECRETPORT"));
    // The placeholders --print-config writes are the only accepted values.
    EXPECT(errorOf(R"({"password_hash": "<redacted>"})").empty());
    EXPECT(errorOf(R"({"password_hash": "<none>"})").empty());
}
TEST_CASE("A rejected document changes nothing") {
    Settings settings;
    applyConfigText(R"({"port": 1234})", settings);
    EXPECT(!contains(
        errorOf(R"({"port": 4321, "rate_limit": {"max_failurs": 1}})"),
        "unreachable"));
    try {
        applyConfigText(R"({"port": 4321, "max_connections": 0})", settings);
        EXPECT(false);
    }
    catch (const ConfigError &) {
    }
    EXPECT_EQ(settings.server.port, 1234);
    try {
        applyConfigText(
            R"({"port": 4321, "rate_limit": {"lockout_initial_seconds": 9, "lockout_max_seconds": 5}})",
            settings);
        EXPECT(false);
    }
    catch (const ConfigError &) {
    }
    EXPECT_EQ(settings.server.port, 1234);
}
TEST_CASE("The documented example files load") {
    for (const char *name : {"server-laptop.json", "server-public.json"}) {
        const auto text =
            slurp(std::string(MATTERM_SOURCE_DIR) + "/docs/examples/" + name);
        EXPECT(!text.empty());
        Settings settings;
        applyConfigText(text, settings, name);
        EXPECT_EQ(settings.server.process.arguments[0], "/bin/bash");
    }
    Settings pub;
    applyConfigText(slurp(std::string(MATTERM_SOURCE_DIR) +
                          "/docs/examples/server-public.json"),
                    pub);
    EXPECT(!pub.server.rateLimit.exemptLoopback);
    EXPECT_EQ(pub.server.rateLimit.maxFailures, 3u);
    EXPECT_EQ(pub.credentialsPath, "/etc/matterm/credentials");
}
TEST_CASE("Config files must be private regular files") {
    TempDir dir;
    EXPECT(!dir.path.empty());
    Settings settings;
    const auto good = dir.write("good.json", R"({"port": 4000})");
    EXPECT(loadConfigFile(good, ConfigFile::Required, settings));
    EXPECT_EQ(settings.server.port, 4000);
    // Readable by others is fine; writable by group or others is not.
    const auto readable = dir.write("readable.json", "{}", 0644);
    EXPECT(loadConfigFile(readable, ConfigFile::Required, settings));
    for (const mode_t mode : {mode_t(0620), mode_t(0602), mode_t(0666)}) {
        const auto bad = dir.write("bad.json", "{}", mode);
        try {
            loadConfigFile(bad, ConfigFile::Required, settings);
            EXPECT(false);
        }
        catch (const ConfigError &error) {
            EXPECT(contains(error.what(), "writable by group or others"));
            EXPECT(contains(error.what(), bad));
        }
    }
    // A symbolic link is refused even when it points at a good file.
    const auto link = dir.path + "/link.json";
    EXPECT_EQ(::symlink(good.c_str(), link.c_str()), 0);
    try {
        loadConfigFile(link, ConfigFile::Required, settings);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "symbolic link"));
    }
    // Directories and files of other users are refused.
    try {
        loadConfigFile(dir.path, ConfigFile::Required, settings);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "not a regular file"));
    }
    if (::geteuid() != 0 && ::access("/etc/passwd", R_OK) == 0) {
        try {
            loadConfigFile("/etc/passwd", ConfigFile::Required, settings);
            EXPECT(false);
        }
        catch (const ConfigError &error) {
            EXPECT(contains(error.what(), "not owned by the current user"));
        }
    }
    // Missing: fine when optional, an error when required.
    EXPECT(!loadConfigFile(
        dir.path + "/none.json", ConfigFile::Optional, settings));
    try {
        loadConfigFile(dir.path + "/none.json", ConfigFile::Required, settings);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "none.json: cannot open"));
    }
    // A bad optional file is still an error; errors carry the file name.
    const auto broken = dir.write("broken.json", "{\"port\": ");
    try {
        loadConfigFile(broken, ConfigFile::Optional, settings);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), broken + ":1:"));
    }
    // Size is bounded.
    const auto huge = dir.write("huge.json", std::string(70000, ' ') + "{}");
    try {
        loadConfigFile(huge, ConfigFile::Required, settings);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "too large"));
    }
}
TEST_CASE("Default paths follow XDG, then HOME, and only absolute values") {
    const auto env = [](const char *xdg, const char *home) {
        return [=](const char *name) -> const char * {
            return std::string(name) == "XDG_CONFIG_HOME" ? xdg : home;
        };
    };
    EXPECT_EQ(defaultConfigDirectory(env("/x/cfg", "/home/u")),
              "/x/cfg/matterm");
    EXPECT_EQ(defaultConfigDirectory(env(nullptr, "/home/u")),
              "/home/u/.config/matterm");
    EXPECT_EQ(defaultConfigDirectory(env("", "/home/u")),
              "/home/u/.config/matterm");
    EXPECT_EQ(defaultConfigDirectory(env("relative", "/home/u")),
              "/home/u/.config/matterm");
    EXPECT_EQ(defaultConfigDirectory(env(nullptr, "relative")), "");
    EXPECT_EQ(defaultConfigDirectory(env(nullptr, nullptr)), "");
}
TEST_CASE("Precedence: defaults, then the file, then the flags") {
    TempDir dir;
    const auto home = dir.path + "/home";
    const auto config = home + "/matterm";
    ::mkdir(home.c_str(), 0700);
    ::mkdir(config.c_str(), 0700);
    const auto env = [&](const char *name) -> const char * {
        return std::string(name) == "XDG_CONFIG_HOME" ? home.c_str() : nullptr;
    };
    // No file: defaults, and the credentials sit next to where the file would
    // be.
    {
        const auto settings = resolveSettings(parseCommandLine({}), env);
        EXPECT_EQ(settings.server.port, 8080);
        EXPECT_EQ(settings.credentialsPath, config + "/credentials");
        EXPECT(settings.credentialsDefaulted);
    }
    // The default file applies; flags beat it.
    std::ofstream(config + "/server.json") << R"({
        "port": 9000, "allowed_origin": "http://localhost:9000",
        "rate_limit": {"max_failures": 2}, "static_dir": "/from/file"})";
    ::chmod((config + "/server.json").c_str(), 0600);
    {
        const auto settings = resolveSettings(parseCommandLine({}), env);
        EXPECT_EQ(settings.server.port, 9000);
        EXPECT_EQ(settings.server.rateLimit.maxFailures, 2u);
    }
    {
        const auto settings =
            resolveSettings(parseCommandLine({"--port",
                                              "9100",
                                              "--static-dir",
                                              "/from/flag",
                                              "--credentials",
                                              "/c/creds",
                                              "--allow-plain-http",
                                              "--listen",
                                              "0.0.0.0",
                                              "--",
                                              "/bin/zsh",
                                              "-i"}),
                            env);
        EXPECT_EQ(settings.server.port, 9100);
        EXPECT_EQ(settings.server.staticDir, "/from/flag");
        EXPECT_EQ(settings.server.allowedOrigin,
                  "http://localhost:9000"); // From the file.
        EXPECT_EQ(settings.server.address, "0.0.0.0");
        EXPECT(settings.server.allowPlainHttp);
        EXPECT_EQ(settings.credentialsPath, "/c/creds");
        EXPECT(!settings.credentialsDefaulted);
        EXPECT_EQ(settings.server.process.arguments.size(), std::size_t(2));
        EXPECT_EQ(settings.server.process.arguments[0], "/bin/zsh");
    }
    // --config names another file, which is required and gives the credentials
    // directory.
    const auto other = dir.write(
        "other.json", R"({"port": 9200, "credentials": "/from/file/creds"})");
    {
        const auto settings =
            resolveSettings(parseCommandLine({"--config", other}), env);
        EXPECT_EQ(settings.server.port, 9200);
        EXPECT_EQ(settings.credentialsPath, "/from/file/creds");
        EXPECT(!settings.credentialsDefaulted);
        // The default file was not read.
        EXPECT_EQ(settings.server.rateLimit.maxFailures, 5u);
    }
    {
        const auto plain = dir.write("plain.json", "{}");
        const auto settings =
            resolveSettings(parseCommandLine({"--config", plain}), env);
        EXPECT_EQ(settings.credentialsPath, dir.path + "/credentials");
    }
    try {
        resolveSettings(
            parseCommandLine({"--config", dir.path + "/absent.json"}), env);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "absent.json"));
    }
    // The flags are validated like the file.
    try {
        resolveSettings(parseCommandLine({"--origin", "http://x/y"}), env);
        EXPECT(false);
    }
    catch (const ConfigError &error) {
        EXPECT(contains(error.what(), "allowed_origin"));
    }
    // Without a usable home there is no default file and no default
    // credentials.
    const auto none = resolveSettings(parseCommandLine({}), noEnvironment);
    EXPECT(none.credentialsPath.empty());
}
TEST_CASE("Command-line parsing") {
    const auto cl = parseCommandLine({"--config",
                                      "c.json",
                                      "--print-config",
                                      "--set-password",
                                      "--help",
                                      "--origin",
                                      "http://o",
                                      "--listen",
                                      "::1",
                                      "--port",
                                      "0"});
    EXPECT_EQ(*cl.configPath, "c.json");
    EXPECT(cl.printConfig && cl.setPassword && cl.help);
    EXPECT_EQ(*cl.origin, "http://o");
    EXPECT_EQ(*cl.listen, "::1");
    EXPECT_EQ(*cl.port, 0);
    EXPECT(!cl.program);
    // Everything after -- is the program, flags included.
    const auto program = parseCommandLine({"--", "/bin/ls", "--port", "x"});
    EXPECT_EQ(program.program->size(), std::size_t(3));
    for (const auto &bad :
         std::vector<std::vector<std::string>>{{"--port"},
                                               {"--port", "x"},
                                               {"--port", "65536"},
                                               {"--port", "-1"},
                                               {"--port", "80x"},
                                               {"--config"},
                                               {"--"},
                                               {"--nope"},
                                               {"positional"}}) {
        try {
            parseCommandLine(bad);
            EXPECT(false);
        }
        catch (const ConfigError &) {
        }
    }
}
TEST_CASE("--print-config round-trips and hides the hash") {
    Settings original;
    original.credentialsPath = "/c/credentials";
    original.server.passwordHash =
        "$argon2id$v=19$m=65536,t=2,p=1$SECRETSECRET";
    original.server.address = "::1";
    original.server.port = 0;
    original.server.staticDir = "/srv/\"web\"";
    original.server.process.arguments = {
        "/bin/sh", "-c", "echo 'a\tb'\n\x01 \xc3\xa9"};
    original.server.process.environmentAllowList = {};
    original.server.rateLimit.exemptLoopback = false;
    original.server.rateLimit.globalAttemptsPerHour = 0;
    original.server.session.outputWindow = 65536;
    const auto text = printConfig(original);
    EXPECT(!contains(text, "SECRET"));
    EXPECT(!contains(text, "argon2"));
    EXPECT(contains(text, "\"password_hash\": \"<redacted>\""));
    EXPECT(contains(text, "\"environment_allow_list\": []"));
    Settings loaded;
    applyConfigText(text, loaded, "printed");
    EXPECT(loaded.server.passwordHash.empty()); // The placeholder sets nothing.
    EXPECT_EQ(loaded.credentialsPath, "/c/credentials");
    EXPECT_EQ(loaded.server.address, "::1");
    EXPECT_EQ(loaded.server.staticDir, "/srv/\"web\"");
    EXPECT(loaded.server.process.arguments ==
           original.server.process.arguments);
    EXPECT(loaded.server.process.environmentAllowList.empty());
    EXPECT(!loaded.server.rateLimit.exemptLoopback);
    EXPECT_EQ(loaded.server.session.outputWindow, std::size_t(65536));
    // Printing the loaded settings again gives the same text (apart from the
    // hash line).
    loaded.server.passwordHash = original.server.passwordHash;
    EXPECT_EQ(printConfig(loaded), text);
    // Defaults round-trip too, and an unset hash prints its own placeholder.
    Settings defaults;
    const auto defaultText = printConfig(defaults);
    EXPECT(contains(defaultText, "\"password_hash\": \"<none>\""));
    Settings again;
    applyConfigText(defaultText, again);
    EXPECT_EQ(printConfig(again), defaultText);
}
TEST_SUIT_END
