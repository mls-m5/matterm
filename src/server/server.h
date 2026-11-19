#pragma once
#include "auth.h"
#include "ptysessionhost.h"
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
    RateLimitConfig rateLimit;
    AuthConfig auth;
    ProcessConfig process;
    unsigned maxUnauthenticated = 16;
    unsigned maxConnections = 32;
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
