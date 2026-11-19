// Shutdown and deadline behavior of the server with real sockets and PTYs.
#include "mls-unit-test/unittest.h"
#include "serverfixture.h"

namespace {
// Logs in, upgrades and reads until the child announced its pid.
int startSession(Fixture &fixture, Client &client, int receiveBuffer = 0) {
    client.connect(fixture,
                   fixture.login(),
                   "http://localhost",
                   "matterm-tty.v1",
                   receiveBuffer);
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    if (!std::holds_alternative<protocol::Welcome>(client.read()))
        return -1;
    return childPid(client.initialOutput());
}
// True when a plain request is served again, that is, the connection slot
// (the fixture allows exactly one) was released before the deadline.
bool slotReleased(Fixture &fixture, std::chrono::milliseconds limit) {
    const auto deadline = server::Clock::now() + limit;
    while (server::Clock::now() < deadline) {
        try {
            if (fixture.request(http::verb::get, "/").result() ==
                http::status::ok)
                return true;
        }
        catch (const std::exception &) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
void shortDeadlines(server::ServerConfig &config) {
    using std::chrono::milliseconds;
    config.maxConnections = 1;
    config.session.ackStallTimeout = milliseconds(1000);
    config.session.idleTimeout = milliseconds(2000);
    config.webSocketHandshakeTimeout = milliseconds(2000);
    config.webSocketIdleTimeout = milliseconds(3000);
}
void floodWindow(server::ServerConfig &config) {
    using std::chrono::seconds;
    config.maxConnections = 1;
    // Every timer is far away, so only the server's own send queue limit
    // (512 KiB, 1024 messages) can end the connection within the test.
    config.session.ackStallTimeout = seconds(60);
    config.session.idleTimeout = seconds(60);
    config.webSocketIdleTimeout = seconds(60);
    // Far beyond what the kernel buffers hold.
    config.session.outputWindow = 64u << 20;
}
void shortRequest(server::ServerConfig &config) {
    config.maxConnections = 1;
    config.requestTimeout = std::chrono::milliseconds(500);
}
} // namespace
TEST_SUIT_BEGIN(ServerLifecycle)
TEST_CASE("Shutdown with an active session kills and reaps the shell") {
    Client client;
    int pid = -1;
    {
        Fixture fixture({"/bin/sh",
                         "-c",
                         "trap '' HUP; printf 'CHILD:%s\\n' \"$$\"; while :; "
                         "do sleep 1; done"});
        pid = startSession(fixture, client);
        EXPECT(pid > 0);
    } // io.stop(), then the server and its connections are destroyed.
    EXPECT(gone(pid));
    beast::flat_buffer buffer;
    beast::error_code ec;
    for (unsigned i = 0; i < 100 && !ec; ++i)
        client.socket.read(buffer, ec);
    EXPECT(ec);
}
TEST_CASE("Shutdown while the child floods output") {
    Client client;
    int pid = -1;
    {
        Fixture fixture(
            {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec yes flood"});
        pid = startSession(fixture, client);
        EXPECT(pid > 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT(gone(pid));
}
TEST_CASE("Shutdown with unauthenticated and half-open connections") {
    net::io_context clientIo;
    tcp::socket silent(clientIo), partial(clientIo);
    {
        Fixture fixture;
        silent.connect({net::ip::make_address("127.0.0.1"), fixture.port});
        partial.connect({net::ip::make_address("127.0.0.1"), fixture.port});
        net::write(partial,
                   net::buffer(std::string("POST /login HTTP/1.1\r\nHo")));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Reaching this point without hanging is the test; the peers see EOF.
    char byte;
    boost::system::error_code ec;
    silent.read_some(net::buffer(&byte, 1), ec);
    EXPECT(ec);
}
TEST_CASE("A healthy session holds the only connection slot") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec sh"},
        [](server::ServerConfig &config) { config.maxConnections = 1; });
    Client client;
    EXPECT(startSession(fixture, client) > 0);
    EXPECT(!slotReleased(fixture, std::chrono::milliseconds(500)));
}
TEST_CASE(
    "A client that never reads or acknowledges is dropped and cleaned up") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec yes flood"},
        shortDeadlines);
    Client client;
    const int pid = startSession(fixture, client, 1024);
    EXPECT(pid > 0);
    // Acknowledgement stall, then a closing handshake the peer never answers.
    EXPECT(slotReleased(fixture, std::chrono::seconds(15)));
    EXPECT(gone(pid));
}
TEST_CASE("A flood to a client that never reads hits the queue bound") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec yes flood"},
        floodWindow);
    Client client;
    const int pid = startSession(fixture, client, 1024);
    EXPECT(pid > 0);
    EXPECT(slotReleased(fixture, std::chrono::seconds(15)));
    EXPECT(gone(pid));
}
TEST_CASE("WebSocket pings do not extend the closing deadline") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec yes flood"},
        shortDeadlines);
    Client client;
    const int pid = startSession(fixture, client, 1024);
    EXPECT(pid > 0);
    std::atomic<bool> stop{false};
    std::thread chatter([&] {
        boost::system::error_code ec;
        while (!stop && !ec) {
            client.socket.ping({}, ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const bool released = slotReleased(fixture, std::chrono::seconds(15));
    stop = true;
    chatter.join();
    EXPECT(released);
    EXPECT(gone(pid));
}
TEST_CASE("A silent or trickling unauthenticated connection is dropped") {
    Fixture fixture({"/bin/sh"}, shortRequest);
    net::io_context clientIo;
    tcp::socket silent(clientIo);
    silent.connect({net::ip::make_address("127.0.0.1"), fixture.port});
    EXPECT(slotReleased(fixture, std::chrono::seconds(5)));
    // The deadline is absolute: sending a byte now and then does not extend it.
    tcp::socket trickle(clientIo);
    trickle.connect({net::ip::make_address("127.0.0.1"), fixture.port});
    const auto start = server::Clock::now();
    boost::system::error_code ec;
    while (!ec && server::Clock::now() - start < std::chrono::seconds(5)) {
        net::write(trickle, net::buffer(std::string("X-Slow: a\r\n")), ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT(server::Clock::now() - start < std::chrono::seconds(3));
    EXPECT(ec);
}
TEST_SUIT_END
