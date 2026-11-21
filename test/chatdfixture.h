#pragma once
// Shared helpers for the chat server's security tests: a chatd on free loopback
// ports running on its own thread, a blocking line client for the plain TCP
// test listener, and a Beast WebSocket client that can set headers and a source
// address. Include from one translation unit per test binary.
#include "chatserver.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;

const char *kOrigin = "https://chat.example.org";

struct Running {
    explicit Running(chatd::Options options) {
        options.origins = {kOrigin};
        options.tcpEnabled = true;
        server = std::make_unique<chatd::ChatServer>(io, options);
        server->start();
        thread = std::thread([this] { io.run(); });
    }
    ~Running() {
        net::post(io, [this] { server->stop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        io.stop();
        thread.join();
    }
    std::string console(const std::string &command) {
        std::promise<std::string> promise;
        auto future = promise.get_future();
        net::post(io, [&] { promise.set_value(server->console(command)); });
        return future.get();
    }
    net::io_context io;
    std::unique_ptr<chatd::ChatServer> server;
    std::thread thread;
};

inline void setReadTimeout(int fd, int seconds) {
    timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// A blocking client for the plain TCP test listener, with a read timeout.
class Line {
public:
    explicit Line(std::uint16_t port, int timeoutSeconds = 2)
        : socket_(io_) {
        socket_.connect(
            tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        setReadTimeout(socket_.native_handle(), timeoutSeconds);
    }
    void send(const std::string &line) {
        const std::string data = line + "\n";
        net::write(socket_, net::buffer(data));
    }
    bool expect(const std::string &prefix) {
        while (true) {
            std::string l;
            if (!readLine(l)) {
                return false;
            }
            seen.push_back(l);
            if (l.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
    }
    bool ends() {
        std::string l;
        while (readLine(l)) {
            seen.push_back(l);
        }
        return eof_;
    }
    std::vector<std::string> seen;

private:
    bool readLine(std::string &out) {
        while (true) {
            const auto nl = buffer_.find('\n');
            if (nl != std::string::npos) {
                out = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                return true;
            }
            char chunk[512];
            boost::system::error_code ec;
            const auto n = socket_.read_some(net::buffer(chunk), ec);
            if (ec) {
                eof_ =
                    ec == net::error::eof || ec == net::error::connection_reset;
                return false;
            }
            buffer_.append(chunk, n);
        }
    }
    net::io_context io_;
    tcp::socket socket_;
    std::string buffer_;
    bool eof_ = false;
};

chatd::Options quick() {
    chatd::Options o;
    o.room.lockBackstop = 400;
    o.room.draftForward = 50;
    o.room.maxPerSource = 8; // Everyone is 127.0.0.1 here.
    return o;
}

// A Beast client, to see what a browser (or anything else) would see. Extra
// headers are added to the upgrade request; `from` is the local source address
// (any 127.x.y.z works on Linux).
struct WsClient {
    using Headers = std::vector<std::pair<std::string, std::string>>;
    explicit WsClient(std::uint16_t port,
                      const std::string &origin,
                      Headers extra = {},
                      const std::string &from = {})
        : stream(io) {
        auto &socket = stream.next_layer();
        socket.open(tcp::v4());
        if (!from.empty()) {
            socket.bind(tcp::endpoint(net::ip::make_address(from), 0));
        }
        socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        setReadTimeout(socket.native_handle(), 2);
        stream.set_option(
            ws::stream_base::decorator([origin, extra](ws::request_type &req) {
                if (!origin.empty()) {
                    req.set(http::field::origin, origin);
                }
                for (const auto &[name, value] : extra) {
                    req.insert(name, value);
                }
            }));
    }
    bool handshake() {
        beast::error_code ec;
        stream.handshake("127.0.0.1", "/", ec);
        return !ec;
    }
    std::string read() {
        beast::flat_buffer buffer;
        beast::error_code ec;
        stream.read(buffer, ec);
        return ec ? std::string("<closed>")
                  : beast::buffers_to_string(buffer.data());
    }
    void send(const std::string &line) {
        stream.text(true);
        beast::error_code ec;
        stream.write(net::buffer(line), ec);
    }
    // Reads until a frame starts with `prefix`; false on close or timeout.
    bool expect(const std::string &prefix) {
        for (int i = 0; i < 400; ++i) {
            const auto frame = read();
            if (frame == "<closed>") {
                return false;
            }
            if (frame.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return false;
    }
    net::io_context io;
    ws::stream<tcp::socket> stream;
};

// True when a raw TCP connection to `port` is closed by the server within
// `seconds` (whatever it sent first is discarded).
inline bool closedWithin(tcp::socket &socket, int seconds) {
    setReadTimeout(socket.native_handle(), seconds);
    char chunk[512];
    boost::system::error_code ec;
    while (true) {
        socket.read_some(net::buffer(chunk), ec);
        if (ec) {
            return ec == net::error::eof || ec == net::error::connection_reset;
        }
    }
}
} // namespace
