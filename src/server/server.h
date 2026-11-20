#pragma once
#include "auth.h"
#include "ptysessionhost.h"
#include "sessionregistry.h"
#include <utility>

#include <boost/asio/io_context.hpp>
#include <memory>

namespace server {
struct ServerConfig {
    std::string address = "127.0.0.1";
    unsigned short port = 8080;
    std::string allowedOrigin = "http://localhost:8080";
    bool allowPlainHttp = false;
    std::string passwordHash;
    std::string trustedProxy = "none";
    unsigned trustedProxyHops = 0;
    RateLimitConfig rateLimit;
    AuthConfig auth;
    ProcessConfig process;
    RegistryConfig registry;
    unsigned maxUnauthenticated = 16;
    unsigned maxConnections = 32;
    // Directory of the browser client (the web/ build output). Its regular
    // files, read once at startup, are served by exact name; "/" is
    // index.html. Empty: only a placeholder page is served.
    std::string staticDir;
    // Per-connection protocol limits (hello, idle and acknowledgement
    // timeouts).
    protocol::SessionConfig session;
    // Deadline for an unauthenticated connection to deliver its request and
    // receive the response (slow clients are dropped, however slowly they
    // send).
    std::chrono::milliseconds requestTimeout{5000};
    // WebSocket transport deadlines. The handshake timeout also bounds the
    // closing handshake; the idle timeout fires when nothing is received.
    std::chrono::milliseconds webSocketHandshakeTimeout{5000};
    std::chrono::milliseconds webSocketIdleTimeout{60000};
};
// A single io_context thread owns every connection and the authentication
// state. Construct/start as an unprivileged user; destroy after io_context.run
// exits.
class Server {
public:
    Server(boost::asio::io_context &io, ServerConfig config);
    ~Server();
    void start();
    unsigned short port() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace server
