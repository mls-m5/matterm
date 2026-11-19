#include "config.h"
#include "mls-unit-test/unittest.h"
#include "proxysource.h"
#include "serverfixture.h"

namespace {
void proxyConfig(server::ServerConfig &config, const std::string &json) {
    server::Settings settings;
    settings.server = config;
    server::applyConfigText(json, settings);
    config = settings.server;
    config.rateLimit.maxFailures = 1;
}
http::response<http::string_body> login(Fixture &fixture,
                                        const std::string &forwarded,
                                        const std::string &value = "wrong",
                                        bool duplicate = false) {
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({net::ip::make_address("127.0.0.1"), fixture.port});
    http::request<http::string_body> request(http::verb::post, "/login", 11);
    request.set(http::field::host, "localhost");
    request.set(http::field::origin, "http://localhost");
    request.set("X-Forwarded-For", forwarded);
    if (duplicate)
        request.insert("X-Forwarded-For", "203.0.113.250");
    request.body() = "{\"password\":\"" + value + "\"}";
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response;
}
bool rejected(const std::string &json) {
    try {
        server::Settings settings;
        server::applyConfigText(json, settings);
        return false;
    }
    catch (const server::ConfigError &) {
        return true;
    }
}
} // namespace
TEST_SUIT_BEGIN(ServerProxySecurity)
TEST_CASE(
    "Forwarding is opt-in and hop selection fails closed on a short chain") {
    const auto loopback = net::ip::make_address("127.0.0.1");
    server::ProxySource disabled;
    EXPECT(disabled.resolve(loopback, "203.0.113.8").value() == loopback);
    server::ProxySource proxy("127.0.0.1", 1);
    EXPECT_FALSE(proxy.resolve(loopback, "203.0.113.8").has_value());
    EXPECT(proxy.resolve(loopback, std::nullopt).value() == loopback);
    EXPECT(proxy.trusts(net::ip::make_address("::ffff:127.0.0.1")));
    EXPECT_FALSE(proxy.trusts(net::ip::make_address("127.0.0.2")));
    EXPECT(proxy.resolve(loopback, " \t203.0.113.8\t, 192.0.2.2 ").value() ==
           net::ip::make_address("203.0.113.8"));
    std::string many = "203.0.113.8";
    for (unsigned i = 1; i < 33; ++i)
        many += ",192.0.2.2";
    EXPECT_FALSE(proxy.resolve(loopback, many).has_value());
    server::ProxySource any("any");
    EXPECT(any.resolve(net::ip::make_address("192.0.2.2"), "203.0.113.8")
               .value() == net::ip::make_address("203.0.113.8"));
}
TEST_CASE("Untrusted peers cannot rotate lockout keys with spoofed or "
          "duplicate headers") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config, R"({"trusted_proxy":"192.0.2.1"})");
    });
    EXPECT(login(fixture, "198.51.100.1").result() ==
           http::status::unauthorized);
    EXPECT(login(fixture, "198.51.100.2", password, true).result() ==
           http::status::too_many_requests);
}
TEST_CASE("Last proxy-appended hop wins, not an attacker-controlled prefix") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config, R"({"trusted_proxy":"127.0.0.1"})");
    });
    EXPECT(login(fixture, "198.51.100.1, 203.0.113.8").result() ==
           http::status::unauthorized);
    EXPECT(login(fixture, "198.51.100.2, 203.0.113.8", password).result() ==
           http::status::too_many_requests);
    EXPECT(login(fixture, "198.51.100.1, 203.0.113.9", password).result() ==
           http::status::ok);
}
TEST_CASE("Configured hops skip only the explicitly trusted proxy chain") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config,
                    R"({"trusted_proxy":"127.0.0.1","trusted_proxy_hops":1})");
    });
    EXPECT(login(fixture, "198.51.100.1, 203.0.113.8, 192.0.2.2").result() ==
           http::status::unauthorized);
    EXPECT(login(fixture, "198.51.100.2, 203.0.113.8, 192.0.2.2", password)
               .result() == http::status::too_many_requests);
    EXPECT(login(fixture, "203.0.113.9, 192.0.2.2", password).result() ==
           http::status::ok);
}
TEST_CASE("Forwarded IPv6 sources share one lockout per /64") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config, R"({"trusted_proxy":"127.0.0.1"})");
    });
    EXPECT(login(fixture, "2001:db8:42::1").result() ==
           http::status::unauthorized);
    EXPECT(login(fixture, "2001:db8:42::2", password).result() ==
           http::status::too_many_requests);
    EXPECT(login(fixture, "2001:db8:43::1", password).result() ==
           http::status::ok);
}
TEST_CASE("Malformed and duplicate forwarding from a trusted proxy fail before "
          "hashing") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config, R"({"trusted_proxy":"127.0.0.1"})");
        config.rateLimit.globalAttemptsPerHour = 1;
        config.rateLimit.exemptLoopback = false;
    });
    for (const auto *value : {"",
                              "unknown",
                              "203.0.113.8,",
                              ",203.0.113.8",
                              "203.0.113.8,,192.0.2.1",
                              "203.0.113.8:80",
                              "[::1]",
                              "fe80::1%lo",
                              "spoof,203.0.113.8"}) {
        EXPECT(login(fixture, value).result() == http::status::bad_request);
    }
    EXPECT(login(fixture, "203.0.113.8", "wrong", true).result() ==
           http::status::bad_request);
    EXPECT(login(fixture, "203.0.113.8", password).result() ==
           http::status::ok);
}
TEST_CASE("Forwarded external clients do not inherit a loopback global-budget "
          "exemption") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        proxyConfig(config, R"({"trusted_proxy":"127.0.0.1"})");
        config.rateLimit.globalAttemptsPerHour = 1;
    });
    EXPECT(login(fixture, "203.0.113.8").result() ==
           http::status::unauthorized);
    EXPECT(login(fixture, "203.0.113.9", password).result() ==
           http::status::too_many_requests);
}
TEST_CASE(
    "Proxy configuration validates addresses and hop bounds and round-trips") {
    for (const auto *json : {R"({"trusted_proxy":"bad"})",
                             R"({"trusted_proxy":"127.0.0.1/8"})",
                             R"({"trusted_proxy":"fe80::1%lo"})",
                             R"({"trusted_proxy_hops":-1})",
                             R"({"trusted_proxy_hops":17})"}) {
        EXPECT(rejected(json));
    }
    server::Settings settings;
    server::applyConfigText(R"({"trusted_proxy":"any","trusted_proxy_hops":2})",
                            settings);
    const auto printed = server::printConfig(settings);
    EXPECT(printed.find("\"trusted_proxy\": \"any\"") != std::string::npos);
    server::Settings restored;
    server::applyConfigText(printed, restored);
    EXPECT_EQ(server::printConfig(restored), printed);
    const auto args = server::parseCommandLine(
        {"--trusted-proxy", "127.0.0.1", "--trusted-proxy-hops", "1"});
    server::applyCommandLine(args, restored);
    const auto overridden = server::printConfig(restored);
    EXPECT(overridden.find("\"trusted_proxy\": \"127.0.0.1\"") !=
           std::string::npos);
    EXPECT(overridden.find("\"trusted_proxy_hops\": 1") != std::string::npos);
}
TEST_SUIT_END
