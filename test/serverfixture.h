#pragma once
// Shared fixture for the server tests that talk to a real, in-process server
// over loopback sockets. Include it from one translation unit per test binary.
#include "server.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cerrno>
#include <csignal>
#include <functional>
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
    Fixture(std::vector<std::string> arguments =
                {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
            std::function<void(server::ServerConfig &)> tune = {}) {
        server::ServerConfig config;
        config.port = 0;
        config.allowedOrigin = "http://localhost";
        static const auto hash = server::hashPassword(password);
        config.passwordHash = hash;
        config.process.arguments = std::move(arguments);
        if (tune)
            tune(config);
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
                 std::string subprotocol = "matterm-tty.v1",
                 int receiveBuffer = 0) {
        if (receiveBuffer) {
            // A tiny receive window makes server writes stall quickly.
            auto &raw = socket.next_layer().socket();
            raw.open(tcp::v4());
            raw.set_option(
                net::socket_base::receive_buffer_size(receiveBuffer));
        }
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
inline int childPid(const std::string &output) {
    const auto at = output.find("CHILD:");
    if (at == std::string::npos)
        return -1;
    return std::stoi(output.substr(at + 6));
}
inline bool gone(int pid) {
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
