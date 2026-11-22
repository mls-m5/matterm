#pragma once
// Fixture for tests of matterm-server together with a real daemon, both in temp directories.
// Include it from one translation unit per test binary.
#include "daemonfixture.h"
#include "serverfixture.h"

namespace {
using namespace std::chrono_literals;

struct WithDaemon {
    Running daemon;
    std::unique_ptr<Fixture> web;
    explicit WithDaemon(bool expose = true, std::string socket = {},
                        std::function<void(server::ServerConfig &)> tuneServer = {},
                        std::function<void(mtd::DaemonConfig &)> tuneDaemon = {})
        : daemon(60, std::nullopt, {}, std::move(tuneDaemon)) {
        const auto path = socket.empty() ? daemon.path : socket;
        web = std::make_unique<Fixture>(std::vector<std::string>{"/bin/sh"}, [&](server::ServerConfig &c) {
            c.exposeLocalSessions = expose;
            c.daemonSocket = expose ? path : std::string();
            if (tuneServer) tuneServer(c);
        });
    }
};

http::response<http::string_body> post(Fixture &f, const std::string &target, const std::string &cookie) {
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({net::ip::make_address("127.0.0.1"), f.port});
    http::request<http::string_body> request(http::verb::post, target, 11);
    request.set(http::field::host, "localhost");
    request.set(http::field::origin, "http://localhost");
    if (!cookie.empty()) request.set(http::field::cookie, cookie);
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response;
}

protocol::Hello localHello(const std::string &id) {
    protocol::Hello h;
    h.sessionId = "local:" + id;
    h.features = protocol::featureReplay;
    return h;
}

// Next server message, or nullopt when the connection ends.
std::optional<protocol::ServerMessage> next(Client &c) {
    try {
        return c.read();
    }
    catch (const std::exception &) {
        return std::nullopt;
    }
}
bool closes(Client &c) {
    for (int i = 0; i < 200; ++i) {
        if (!next(c)) return true;
    }
    return false;
}
bool outputHas(Client &c, const std::string &needle) {
    std::string seen;
    for (int i = 0; i < 400 && seen.find(needle) == std::string::npos; ++i) {
        const auto m = next(c);
        if (!m) return false;
        if (const auto *o = std::get_if<protocol::Output>(&*m)) {
            seen += o->data;
            c.send(protocol::Ack{o->offset + o->data.size()});
        }
    }
    return seen.find(needle) != std::string::npos;
}

} // namespace
