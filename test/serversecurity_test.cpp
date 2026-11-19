// Regression tests for docs/SECURITY_REVIEW.md. Each case names the finding.
#include "config.h"
#include "mls-unit-test/unittest.h"
#include "serverfixture.h"
#include <fstream>
#include <regex>
#include <sstream>

namespace {
std::string readFile(const std::string &relative) {
    std::ifstream in(std::string(MATTERM_SOURCE_DIR) + "/" + relative);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
std::string unquote(std::string item) {
    const auto comment = item.find(" #");
    if (comment != std::string::npos)
        item.erase(comment);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\r'))
        item.pop_back();
    if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') &&
        item.back() == item.front())
        item = item.substr(1, item.size() - 2);
    // Compose substitutions: give them a plausible value.
    return std::regex_replace(
        item, std::regex(R"(\$\{[^}]*\})"), "term.example.org");
}
// The argument lists of every `command:` key of a compose file (block lists
// and one-line [a, b] lists).
std::vector<std::vector<std::string>> composeCommands(const std::string &text) {
    std::vector<std::vector<std::string>> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto at = line.find("command:");
        if (at == std::string::npos || line.find_first_not_of(' ') != at)
            continue;
        std::vector<std::string> arguments;
        const auto rest = line.substr(at + 8);
        const auto open = rest.find('[');
        if (open != std::string::npos) {
            std::string inner =
                rest.substr(open + 1, rest.rfind(']') - open - 1);
            std::istringstream parts(inner);
            std::string part;
            while (std::getline(parts, part, ','))
                arguments.push_back(
                    unquote(part.substr(part.find_first_not_of(' '))));
        }
        else {
            while (std::getline(lines, line)) {
                if (line.find_first_not_of(' ') != std::string::npos &&
                    line[line.find_first_not_of(' ')] == '#')
                    continue;
                const auto dash = line.find("- ");
                if (dash == std::string::npos ||
                    line.find_first_not_of(' ') != dash)
                    break;
                arguments.push_back(unquote(line.substr(dash + 2)));
            }
        }
        result.push_back(std::move(arguments));
    }
    return result;
}
std::string rawExchange(unsigned short port, const std::string &bytes) {
    net::io_context io;
    tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), port});
    net::write(socket, net::buffer(bytes));
    std::string reply;
    boost::system::error_code ec;
    char buffer[4096];
    while (true) {
        const auto count = socket.read_some(net::buffer(buffer), ec);
        if (ec)
            break;
        reply.append(buffer, count);
    }
    return reply;
}
} // namespace

TEST_SUIT_BEGIN(ServerSecurity)

TEST_CASE("Every response carries the isolation and caching headers") {
    Fixture fixture;
    const auto check = [](const http::response<http::string_body> &response) {
        EXPECT_EQ(std::string(response["X-Content-Type-Options"]), "nosniff");
        EXPECT_EQ(std::string(response["Cache-Control"]), "no-store");
        EXPECT_EQ(std::string(response["Referrer-Policy"]), "no-referrer");
        EXPECT(std::string(response["Content-Security-Policy"])
                   .find("frame-ancestors 'none'") != std::string::npos);
        // A hostile page that opened us (or that we opened) must not keep a
        // window reference, and nobody else may embed our responses.
        EXPECT_EQ(std::string(response["Cross-Origin-Opener-Policy"]),
                  "same-origin");
        EXPECT_EQ(std::string(response["Cross-Origin-Resource-Policy"]),
                  "same-origin");
    };
    check(fixture.request(http::verb::get, "/"));
    check(fixture.request(http::verb::get, "/nothing"));
    check(fixture.request(
        http::verb::post, "/login", "{}", "http://evil.example"));
    check(fixture.request(http::verb::post, "/login", "{}"));
    check(fixture.request(
        http::verb::post, "/login", "{\"password\":\"" + password + "\"}"));
}

TEST_CASE("Deploy: compose commands are accepted by the real flag parser") {
    for (const char *name :
         {"deploy/podman/compose.yaml", "deploy/podman/compose.server.yaml"}) {
        const auto commands = composeCommands(readFile(name));
        EXPECT(!commands.empty());
        for (const auto &arguments : commands) {
            if (arguments.size() == 1 && arguments[0] == "--set-password")
                continue;
            server::CommandLine parsed;
            bool accepted = true;
            try {
                parsed = server::parseCommandLine(arguments);
                server::Settings settings;
                server::applyCommandLine(parsed, settings);
                server::validateSettings(settings);
            }
            catch (const std::exception &) {
                accepted = false;
            }
            EXPECT(accepted);
            // Without it the container serves only the placeholder page.
            EXPECT(parsed.staticDir.has_value());
        }
    }
}

TEST_CASE("Deploy: the compose files use no flag the server lacks") {
    for (const char *name :
         {"deploy/podman/compose.yaml", "deploy/podman/compose.server.yaml"}) {
        const auto text = readFile(name);
        EXPECT(text.find("--trusted-proxy") == std::string::npos);
        EXPECT(text.find("--allowed-origin") == std::string::npos);
        EXPECT(text.find("--health-check") == std::string::npos);
    }
}

TEST_CASE(
    "Deploy: nginx example limits TLS versions, logins and slow clients") {
    const auto text = readFile("deploy/podman/nginx-host.conf.example");
    EXPECT(text.find("ssl_protocols TLSv1.2 TLSv1.3;") != std::string::npos);
    EXPECT(text.find("limit_req_zone") != std::string::npos);
    EXPECT(text.find("location = /login") != std::string::npos);
    EXPECT(text.find("limit_req zone=") != std::string::npos);
    EXPECT(text.find("client_header_timeout") != std::string::npos);
    EXPECT(text.find("client_body_timeout") != std::string::npos);
    // Unknown Host names never reach the server.
    EXPECT(text.find("default_server") != std::string::npos);
}

// Design decision pending (docs/SECURITY_REVIEW.md, finding 2): behind a
// reverse proxy every client has the proxy's address, so one attacker keeps
// the owner locked out with a single wrong password per lockout period. The
// test names the wanted behavior and stays disabled until `trusted_proxy`
// exists.
TEST_CASE("disabled Lockout of one source does not lock out another") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        config.rateLimit.maxFailures = 1;
    });
    fixture.request(http::verb::post, "/login", "{\"password\":\"x\"}");
    // Both requests come from 127.0.0.1; an X-Forwarded-For-aware server
    // would key the second one differently.
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({net::ip::make_address("127.0.0.1"), fixture.port});
    http::request<http::string_body> request(http::verb::post, "/login", 11);
    request.set(http::field::host, "localhost");
    request.set(http::field::origin, "http://localhost");
    request.set("X-Forwarded-For", "203.0.113.9");
    request.body() = "{\"password\":\"" + password + "\"}";
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    EXPECT(response.result() == http::status::ok);
}

TEST_CASE("Checked: pipelined and smuggled bytes get no second response") {
    Fixture fixture;
    const std::string body = "{\"password\":\"wrong\"}";
    const std::string one = "POST /login HTTP/1.1\r\nHost: localhost\r\n"
                            "Origin: http://localhost\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto reply = rawExchange(fixture.port,
                                   one + "GET / HTTP/1.1\r\n"
                                         "Host: localhost\r\n\r\n");
    EXPECT(reply.find("HTTP/1.1 401") == 0);
    EXPECT(reply.find("HTTP/1.1", 8) == std::string::npos);
    const auto both = rawExchange(
        fixture.port,
        "POST /login HTTP/1.1\r\nHost: localhost\r\nOrigin: "
        "http://localhost\r\n"
        "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT(both.find("HTTP/1.1 200") == std::string::npos);
}

TEST_CASE("Checked: Origin and Host variants are refused") {
    Fixture fixture;
    for (const char *origin : {"null",
                               "http://localhost.",
                               "http://LOCALHOST",
                               "http://localhost:80",
                               "https://localhost",
                               "http://localhost/",
                               "http://localhost@evil.example"}) {
        EXPECT(fixture.request(http::verb::post, "/login", "{}", origin)
                   .result() == http::status::forbidden);
    }
    const auto twice = rawExchange(
        fixture.port,
        "POST /login HTTP/1.1\r\nHost: localhost\r\nHost: evil.example\r\n"
        "Origin: http://localhost\r\nContent-Length: 2\r\n\r\n{}");
    EXPECT(twice.find("HTTP/1.1 403") == 0);
    const auto none = rawExchange(fixture.port,
                                  "POST /login HTTP/1.1\r\nHost: "
                                  "localhost\r\nContent-Length: 2\r\n\r\n{}");
    EXPECT(none.find("HTTP/1.1 403") == 0);
}

TEST_SUIT_END
