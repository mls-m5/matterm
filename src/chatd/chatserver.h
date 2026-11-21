#pragma once

#include "chatroom.h"

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The chat server: Boost.Asio and Beast around one ChatRoom. It moves bytes and
// time into the room and executes what the room says; every rule lives in the
// room (src/chat). Two transports share the same room: WebSocket text frames
// (behind a TLS-terminating reverse proxy) and, only for local testing, plain
// TCP lines on a loopback address. One thread runs everything. See
// docs/CHAT.md.
namespace chatd {

struct Options {
    // WebSocket listener (disabled when wsHost is empty).
    std::string wsHost = "127.0.0.1";
    std::uint16_t wsPort = 0; // 0: pick a free port (tests).
    bool wsEnabled = true;
    // Exact `Origin` values that may connect, for example
    // "https://example.github.io". At least one is required with WebSocket.
    std::vector<std::string> origins;
    // Who may send X-Forwarded-For (an exact IP, "none" or "any"), as in
    // matterm-server.
    std::string trustProxy = "none";
    // Plain TCP for tests with nc or telnet: loopback only, off by default.
    bool tcpEnabled = false;
    std::string tcpHost = "127.0.0.1";
    std::uint16_t tcpPort = 0;
    // Connections that have not finished the handshake yet.
    std::size_t maxPending = 128;
    // ... and how many of them one peer address may hold (not applied to the
    // trusted proxy, which stands for everybody).
    std::size_t maxPendingPerPeer = 4;
    // Told once about a setup problem (a proxy is forwarding but --trust-proxy
    // is not set). Empty: silent.
    std::function<void(const std::string &)> warn;
    chat::RoomConfig room;
};

class ChatServer {
public:
    // Throws std::invalid_argument for options that are not allowed (a TCP
    // address that is not loopback, WebSocket without an allowed origin, ...)
    // and boost::system::system_error when a port cannot be opened.
    ChatServer(boost::asio::io_context &io, Options options);
    ~ChatServer();
    ChatServer(const ChatServer &) = delete;
    ChatServer &operator=(const ChatServer &) = delete;

    void start();
    void stop();
    std::uint16_t wsPort() const;
    std::uint16_t tcpPort() const;
    // An operator command (the console); returns the text to print. Must be
    // called on the io_context's thread.
    std::string console(std::string_view command);
    chat::ChatRoom::Stats stats() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace chatd
