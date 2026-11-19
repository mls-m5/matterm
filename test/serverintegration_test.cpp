#include "mls-unit-test/unittest.h"
#include "server.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cerrno>
#include <csignal>
#include <thread>
#include <unistd.h>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
const std::string password = "integration test password 123456789";
struct Fixture {
    net::io_context io;
    std::unique_ptr<server::Server> listener;
    std::thread thread;
    unsigned short port;
    Fixture(std::vector<std::string> arguments = {
                "/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"}) {
        server::ServerConfig config;
        config.port = 0;
        config.allowedOrigin = "http://localhost";
        static const auto hash = server::hashPassword(password);
        config.passwordHash = hash;
        config.process.arguments = std::move(arguments);
        listener = std::make_unique<server::Server>(io, config);
        port = listener->port();
        listener->start();
        thread = std::thread([this] { io.run(); });
    }
    ~Fixture() {
        io.stop();
        thread.join();
        listener.reset();
    }
    http::response<http::string_body> request(
        http::verb method,
        std::string target,
        std::string body = {},
        std::string origin = "http://localhost") {
        net::io_context clientIo;
        beast::tcp_stream stream(clientIo);
        stream.connect({net::ip::make_address("127.0.0.1"), port});
        http::request<http::string_body> request(method, target, 11);
        request.set(http::field::host, "localhost");
        request.set(http::field::origin, origin);
        request.body() = std::move(body);
        request.prepare_payload();
        http::write(stream, request);
        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);
        return response;
    }
    std::string login() {
        const auto result = request(
            http::verb::post, "/login", "{\"password\":\"" + password + "\"}");
        const auto field = result[http::field::set_cookie];
        return std::string(field.substr(0, field.find(';')));
    }
};
struct Client {
    net::io_context io;
    ws::stream<beast::tcp_stream> socket{io};
    void connect(Fixture &fixture,
                 std::string cookie,
                 std::string origin = "http://localhost",
                 std::string subprotocol = "matterm-tty.v1") {
        socket.next_layer().connect(
            {net::ip::make_address("127.0.0.1"), fixture.port});
        socket.set_option(
            ws::stream_base::decorator([=](ws::request_type &request) {
                request.set(http::field::cookie, cookie);
                request.set(http::field::origin, origin);
                request.set(http::field::sec_websocket_protocol, subprotocol);
            }));
        socket.handshake("localhost", "/ws");
        socket.binary(true);
    }
    void send(const protocol::ClientMessage &message) {
        std::string encoded;
        if (!protocol::encode(message, encoded))
            throw std::runtime_error("encode");
        socket.write(net::buffer(encoded));
    }
    protocol::ServerMessage read() {
        beast::flat_buffer buffer;
        socket.read(buffer);
        auto decoded =
            protocol::decodeServer(beast::buffers_to_string(buffer.data()));
        if (!decoded)
            throw std::runtime_error("decode");
        return *decoded.message;
    }
    std::string initialOutput() {
        std::string output;
        for (unsigned i = 0; i < 20 && output.find('\n') == std::string::npos;
             ++i) {
            auto message = read();
            if (const auto *bytes = std::get_if<protocol::Output>(&message)) {
                output += bytes->data;
                send(protocol::Ack{bytes->offset + bytes->data.size()});
            }
        }
        return output;
    }
};
int childPid(const std::string &output) {
    const auto at = output.find("CHILD:");
    if (at == std::string::npos)
        return -1;
    return std::stoi(output.substr(at + 6));
}
bool gone(int pid) {
    if (pid <= 0)
        return false;
    const auto deadline = server::Clock::now() + std::chrono::seconds(2);
    do {
        if (::kill(pid, 0) == -1 && errno == ESRCH)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (server::Clock::now() < deadline);
    return false;
}
} // namespace
TEST_SUIT_BEGIN(ServerIntegration)
TEST_CASE("Login errors have the same shape and Origin is strict") {
    Fixture fixture;
    auto wrong =
        fixture.request(http::verb::post, "/login", "{\"password\":\"wrong\"}");
    auto malformed = fixture.request(http::verb::post, "/login", "bad json");
    EXPECT(wrong.result() == http::status::unauthorized);
    EXPECT_EQ(wrong.body(), malformed.body());
    EXPECT(wrong.result() == malformed.result());
    EXPECT(
        fixture.request(http::verb::post, "/login", "{}", "http://evil.example")
            .result() == http::status::forbidden);
    EXPECT(fixture.request(http::verb::get, "/").result() == http::status::ok);
    auto login = fixture.request(
        http::verb::post, "/login", "{\"password\":\"" + password + "\"}");
    EXPECT(login.result() == http::status::ok);
    const std::string cookie(login[http::field::set_cookie]);
    EXPECT(cookie.find("HttpOnly; Secure; SameSite=Strict; Path=/") !=
           std::string::npos);
}
TEST_CASE("Upgrade requires cookie, strict Origin and subprotocol") {
    Fixture fixture;
    const auto cookie = fixture.login();
    for (int which = 0; which < 4; ++which) {
        Client client;
        bool refused = false;
        try {
            client.connect(fixture,
                           which == 0   ? ""
                           : which == 1 ? "matterm_session=forged"
                                        : cookie,
                           which == 2 ? "http://evil.example"
                                      : "http://localhost",
                           which == 3 ? "other" : "matterm-tty.v1");
        }
        catch (const boost::system::system_error &) {
            refused = true;
        }
        EXPECT(refused);
    }
}
TEST_CASE("Hello shell output resize and ordered exit, with reaping") {
    Fixture fixture;
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    const auto initial = client.initialOutput();
    const int pid = childPid(initial);
    EXPECT(pid > 0);
    client.send(protocol::Resize{120, 40, 960, 640});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    client.send(protocol::Input{"echo hello; stty size; exit 7\n"});
    std::string output;
    int exitCode = -999;
    for (unsigned i = 0; i < 100; ++i) {
        auto message = client.read();
        if (auto *bytes = std::get_if<protocol::Output>(&message)) {
            output += bytes->data;
            client.send(protocol::Ack{bytes->offset + bytes->data.size()});
        }
        if (auto *exit = std::get_if<protocol::Exit>(&message)) {
            exitCode = exit->code;
            break;
        }
    }
    EXPECT_EQ(exitCode, 7);
    EXPECT(output.find("\r\nhello\r\n") != std::string::npos);
    EXPECT(output.find("40 120") != std::string::npos);
    EXPECT(gone(pid));
    beast::flat_buffer buffer;
    beast::error_code ec;
    client.socket.read(buffer, ec);
    EXPECT(ec == ws::error::closed);
    EXPECT_EQ(client.socket.reason().code, 1000);
}
TEST_CASE("Oversize and text messages close the transport") {
    for (bool text : {false, true}) {
        Fixture fixture;
        Client client;
        client.connect(fixture, fixture.login());
        client.socket.binary(!text);
        const std::string bytes(text ? 1 : 16385, 'x');
        client.socket.write(net::buffer(bytes));
        beast::flat_buffer buffer;
        beast::error_code ec;
        client.socket.read(buffer, ec);
        EXPECT(ec == ws::error::closed);
        EXPECT_EQ(client.socket.reason().code, (text ? 1002 : 1009));
    }
}
TEST_CASE("Disconnect kills and reaps a HUP-ignoring shell") {
    Fixture fixture({"/bin/sh",
                     "-c",
                     "trap '' HUP; printf 'CHILD:%s\\n' \"$$\"; while :; do "
                     "sleep 1; done"});
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    const int pid = childPid(client.initialOutput());
    EXPECT(pid > 0);
    beast::error_code ignored;
    client.socket.next_layer().socket().close(ignored);
    EXPECT(gone(pid));
}
TEST_SUIT_END
