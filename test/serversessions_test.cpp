#include "config.h"
#include "mls-unit-test/unittest.h"
#include "serverfixture.h"

namespace {
void persistent(server::ServerConfig &config) {
    server::Settings settings;
    settings.server = config;
    server::applyConfigText(
        R"({"persistent_sessions":true,"replay_buffer_bytes":65536,"replay_total_bytes":131072,"max_upgrades_per_login":8})",
        settings);
    config = settings.server;
    config.process.terminationGrace = std::chrono::milliseconds(0);
}
std::pair<std::string, int> start(Client &client) {
    client.send(protocol::Hello{1, 80, 24, 0, "", protocol::featureReplay});
    auto message = client.read();
    if (!std::holds_alternative<protocol::Welcome>(message))
        return {"", -1};
    auto id = std::get<protocol::Welcome>(message).sessionId;
    return {id, childPid(client.initialOutput())};
}
void drop(Client &client) {
    beast::error_code ec;
    client.socket.next_layer().socket().shutdown(tcp::socket::shutdown_both,
                                                 ec);
    client.socket.next_layer().socket().close(ec);
}
http::response<http::string_body> action(
    Fixture &fixture,
    std::string cookie,
    std::string target,
    std::string origin = "http://localhost",
    http::verb method = http::verb::post) {
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({net::ip::make_address("127.0.0.1"), fixture.port});
    http::request<http::string_body> request(method, target, 11);
    request.set(http::field::host, "localhost");
    request.set(http::field::origin, origin);
    request.set(http::field::cookie, cookie);
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> result;
    http::read(stream, buffer, result);
    return result;
}
bool sees(Client &client, std::string_view expected) {
    std::string output;
    for (unsigned i = 0; i < 40; ++i) {
        const auto message = client.read();
        if (const auto *bytes = std::get_if<protocol::Output>(&message)) {
            output += bytes->data;
            client.send(protocol::Ack{bytes->offset + bytes->data.size()});
            if (output.find(expected) != std::string::npos)
                return true;
        }
        else if (std::holds_alternative<protocol::Error>(message) ||
                 std::holds_alternative<protocol::Exit>(message))
            return false;
    }
    return false;
}
} // namespace
TEST_SUIT_BEGIN(ServerSessions)
TEST_CASE("A dropped socket reattaches to the same live shell with replay") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto cookie = fixture.login();
    Client first;
    first.connect(fixture, cookie);
    const auto [id, pid] = start(first);
    EXPECT(pid > 0);
    drop(first);
    Client next;
    next.connect(fixture, cookie);
    next.send(protocol::Hello{1, 100, 30, 0, id, protocol::featureReplay});
    const auto welcome = std::get<protocol::Welcome>(next.read());
    EXPECT_EQ(welcome.sessionId, id);
    EXPECT_EQ(welcome.flags, std::uint8_t(3));
    EXPECT_EQ(childPid(next.initialOutput()), pid);
    next.send(protocol::Input{"echo resumed; exit 7\n"});
    int exit = -999;
    for (unsigned i = 0; i < 40; ++i) {
        const auto message = next.read();
        if (auto *bytes = std::get_if<protocol::Output>(&message))
            next.send(protocol::Ack{bytes->offset + bytes->data.size()});
        if (auto *ended = std::get_if<protocol::Exit>(&message)) {
            exit = ended->code;
            break;
        }
    }
    EXPECT_EQ(exit, 7);
    EXPECT(gone(pid));
}
TEST_CASE(
    "Takeover reports Replaced while guessed ids cannot evict a live owner") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto cookie = fixture.login();
    Client first, bad, next;
    first.connect(fixture, cookie);
    const auto [id, pid] = start(first);
    EXPECT(pid > 0);
    bad.connect(fixture, cookie);
    bad.send(protocol::Hello{
        1, 80, 24, 0, std::string(32, '0'), protocol::featureReplay});
    auto unavailable = std::get<protocol::Error>(bad.read());
    EXPECT(unavailable.code == protocol::ErrorCode::ResumeUnsupported);
    EXPECT_EQ(unavailable.reason, std::string("session unavailable"));
    first.send(protocol::Input{"echo original-live\n"});
    EXPECT(sees(first, "original-live"));
    next.connect(fixture, cookie);
    next.send(protocol::Hello{1, 80, 24, 0, id, protocol::featureReplay});
    EXPECT(std::holds_alternative<protocol::Welcome>(next.read()));
    bool replaced = false;
    for (unsigned i = 0; i < 20; ++i) {
        auto message = first.read();
        if (auto *error = std::get_if<protocol::Error>(&message)) {
            replaced = error->code == protocol::ErrorCode::Replaced;
            break;
        }
    }
    EXPECT(replaced);
    EXPECT_EQ(childPid(next.initialOutput()), pid);
}
TEST_CASE("Owner login rotation detaches persistent terminals without "
          "transferring old cookie rights") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto oldCookie = fixture.login();
    Client first;
    first.connect(fixture, oldCookie);
    const auto [id, pid] = start(first);
    const auto cookie = fixture.login();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Client resumed;
    resumed.connect(fixture, cookie);
    resumed.send(protocol::Hello{1, 80, 24, 0, id, protocol::featureReplay});
    EXPECT(std::holds_alternative<protocol::Welcome>(resumed.read()));
    EXPECT_EQ(childPid(resumed.initialOutput()), pid);
    bool refused = false;
    try {
        Client stale;
        stale.connect(fixture, oldCookie);
    }
    catch (const boost::system::system_error &) {
        refused = true;
    }
    EXPECT(refused);
}
TEST_CASE("Kill-all and logout require authenticated same-origin POST and "
          "terminate detached shells") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto cookie = fixture.login();
    Client first;
    first.connect(fixture, cookie);
    const auto [id, pid] = start(first);
    drop(first);
    EXPECT(action(fixture, "matterm_session=forged", "/sessions/close-all")
               .result() == http::status::unauthorized);
    EXPECT(action(fixture, cookie, "/logout", "http://evil.example").result() ==
           http::status::forbidden);
    EXPECT(action(fixture,
                  cookie,
                  "/sessions/close-all",
                  "http://localhost",
                  http::verb::get)
               .result() != http::status::ok);
    EXPECT(action(fixture, cookie, "/sessions/close-all").result() ==
           http::status::ok);
    EXPECT(gone(pid));
    Client second;
    second.connect(fixture, cookie);
    const auto [id2, pid2] = start(second);
    EXPECT(pid2 > 0);
    EXPECT(id2 != id);
    drop(second);
    const auto logout = action(fixture, cookie, "/logout");
    EXPECT(logout.result() == http::status::ok);
    EXPECT(std::string(logout[http::field::set_cookie]).find("Max-Age=0") !=
           std::string::npos);
    EXPECT(gone(pid2));
    bool refused = false;
    try {
        Client stale;
        stale.connect(fixture, cookie);
    }
    catch (const boost::system::system_error &) {
        refused = true;
    }
    EXPECT(refused);
}
TEST_CASE("Concurrent upgrades are bounded and releasing a socket makes its "
          "slot reusable") {
    Fixture fixture({"/bin/sh"}, [](server::ServerConfig &config) {
        server::Settings settings;
        settings.server = config;
        server::applyConfigText(R"({"max_upgrades_per_login":2})", settings);
        config = settings.server;
    });
    const auto cookie = fixture.login();
    Client first, second;
    first.connect(fixture, cookie);
    second.connect(fixture, cookie);
    first.send(protocol::Hello{});
    second.send(protocol::Hello{});
    EXPECT(std::holds_alternative<protocol::Welcome>(first.read()));
    EXPECT(std::holds_alternative<protocol::Welcome>(second.read()));
    Client third;
    third.connect(fixture,
                  cookie); // Pending Hello is bounded by unauthenticated cap.
    third.send(protocol::Hello{});
    const auto error = std::get<protocol::Error>(third.read());
    EXPECT(error.code == protocol::ErrorCode::LimitExceeded);
    drop(first);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Client replacement;
    replacement.connect(fixture, cookie);
    replacement.send(protocol::Hello{});
    EXPECT(std::holds_alternative<protocol::Welcome>(replacement.read()));
}
TEST_SUIT_END
