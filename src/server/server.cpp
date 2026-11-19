#include "server.h"
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <iostream>
#include <sodium.h>
#include <unistd.h>

namespace server {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
namespace {
protocol::Time timeNow() {
    return std::chrono::duration_cast<protocol::Time>(
        Clock::now().time_since_epoch());
}
std::string_view view(beast::string_view value) {
    return {value.data(), value.size()};
}
struct Shared {
    ServerConfig config;
    PasswordVerifier password;
    RateLimiter limiter;
    SessionStore sessions;
    unsigned connections = 0, unauthenticated = 0;
    explicit Shared(ServerConfig cfg)
        : config(std::move(cfg))
        , password(config.passwordHash)
        , limiter(config.rateLimit)
        , sessions(config.auth) {}
};
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(tcp::socket socket, std::shared_ptr<Shared> shared)
        : socket_(std::move(socket))
        , timer_(socket_.get_executor())
        , shared_(std::move(shared))
        , host_(
              shared_->config.process,
              [this](std::string bytes) { send(std::move(bytes)); },
              [this](int code) { close(code); })
        , session_({}, host_) {
        ++shared_->connections;
        ++shared_->unauthenticated;
        const auto address = beast::get_lowest_layer(socket_)
                                 .socket()
                                 .remote_endpoint()
                                 .address();
        source_ = address.to_string();
        loopback_ = address.is_loopback();
        parser_.header_limit(8192);
        parser_.body_limit(512);
    }
    ~Connection() {
        --shared_->connections;
        if (!authenticated_)
            --shared_->unauthenticated;
    }
    void start() {
        beast::get_lowest_layer(socket_).expires_after(std::chrono::seconds(5));
        http::async_read(
            socket_.next_layer(),
            buffer_,
            parser_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (!ec)
                    self->request();
            });
    }

private:
    bool originMatches(const http::request<http::string_body> &request) const {
        // Reject duplicate security headers rather than silently using the
        // first.
        if (request.count(http::field::origin) != 1 ||
            request.count(http::field::host) != 1)
            return false;
        const auto &origin = shared_->config.allowedOrigin;
        const auto scheme = origin.find("://");
        return view(request[http::field::origin]) == origin &&
               scheme != std::string::npos &&
               view(request[http::field::host]) ==
                   std::string_view(origin).substr(scheme + 3);
    }
    void response(http::status status,
                  std::string body,
                  std::string cookie = {},
                  unsigned retry = 0) {
        response_.emplace(status, 11);
        response_->set(http::field::content_type, "text/html; charset=utf-8");
        response_->set(http::field::cache_control, "no-store");
        response_->set(
            "Content-Security-Policy",
            "default-src 'none'; frame-ancestors 'none'; base-uri 'none'");
        response_->set("X-Content-Type-Options", "nosniff");
        if (!cookie.empty())
            response_->set(http::field::set_cookie, cookie);
        if (retry)
            response_->set(http::field::retry_after, std::to_string(retry));
        response_->keep_alive(false);
        response_->body() = std::move(body);
        response_->prepare_payload();
        http::async_write(
            socket_.next_layer(),
            *response_,
            [self = shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code ignored;
                beast::get_lowest_layer(self->socket_)
                    .socket()
                    .shutdown(tcp::socket::shutdown_both, ignored);
            });
    }
    void request() {
        auto &request = parser_.get();
        if (request.method() == http::verb::get && request.target() == "/") {
            response(http::status::ok,
                     "<!doctype html><title>matterm</title><p>matterm server. "
                     "Browser client not installed.</p>");
            return;
        }
        if (!originMatches(request)) {
            response(http::status::forbidden, "Forbidden\n");
            return;
        }
        if (request.method() == http::verb::post &&
            request.target() == "/login") {
            const auto retry = shared_->limiter.attempt(source_, loopback_);
            if (retry) {
                response(http::status::too_many_requests,
                         "Login failed\n",
                         {},
                         retry);
                return;
            }
            auto password = parsePassword(request.body());
            const bool matched = shared_->password.verify(
                password ? std::string_view(*password) : std::string_view(""));
            if (password)
                sodium_memzero(password->data(), password->size());
            sodium_memzero(request.body().data(), request.body().size());
            request.body().clear();
            if (!matched || !password) {
                shared_->limiter.failure(source_);
                std::clog << "Authentication failed\n";
                response(http::status::unauthorized, "Login failed\n");
            }
            else {
                shared_->limiter.success(source_);
                std::clog << "Authentication succeeded\n";
                const auto token = shared_->sessions.issue();
                response(http::status::ok,
                         "Logged in\n",
                         "matterm_session=" + token +
                             "; HttpOnly; Secure; SameSite=Strict; Path=/");
            }
            return;
        }
        if (request.method() == http::verb::get && request.target() == "/ws" &&
            ws::is_upgrade(request)) {
            if (request.count(http::field::cookie) != 1 ||
                request.count(http::field::sec_websocket_protocol) != 1 ||
                view(request[http::field::sec_websocket_protocol]) !=
                    "matterm-tty.v1") {
                response(http::status::unauthorized, "Unauthorized\n");
                return;
            }
            const auto token =
                sessionCookie(view(request[http::field::cookie]));
            if (!token || !shared_->sessions.claim(*token)) {
                response(http::status::unauthorized, "Unauthorized\n");
                return;
            }
            generation_ = shared_->sessions.generation();
            socket_.read_message_max(16384);
            ws::permessage_deflate compression;
            compression.server_enable = false;
            compression.client_enable = false;
            socket_.set_option(compression);
            socket_.set_option(ws::stream_base::timeout{
                std::chrono::seconds(5), std::chrono::seconds(60), false});
            socket_.set_option(
                ws::stream_base::decorator([](ws::response_type &reply) {
                    reply.set(http::field::sec_websocket_protocol,
                              "matterm-tty.v1");
                }));
            socket_.binary(true);
            // No HTTP pipelining/early WS data: avoid losing bytes already
            // read.
            if (buffer_.size()) {
                response(http::status::bad_request, "Bad request\n");
                return;
            }
            beast::get_lowest_layer(socket_).expires_never();
            socket_.async_accept(
                request, [self = shared_from_this()](beast::error_code ec) {
                    if (ec)
                        return;
                    self->authenticated_ = true;
                    --self->shared_->unauthenticated;
                    self->parser_.get() = {};
                    self->session_.start(timeNow());
                    self->read();
                    self->schedule();
                });
            return;
        }
        response(http::status::not_found, "Not found\n");
    }
    void read() {
        socket_.async_read(
            buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->disconnected();
                    return;
                }
                if (!self->socket_.got_binary()) {
                    self->close(1002);
                    return;
                }
                if (!self->shared_->sessions.attachedValid(self->generation_,
                                                           true)) {
                    self->close(1008);
                    return;
                }
                const auto bytes =
                    beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());
                try {
                    self->session_.onMessage(bytes, timeNow());
                }
                catch (const std::exception &) {
                    self->close(1011);
                }
                if (!self->closing_ && !self->dead_)
                    self->read();
            });
    }
    void send(std::string bytes) {
        if (dead_ || closing_)
            return;
        if (bytes.size() > 524288 - queuedBytes_ || queue_.size() >= 1024) {
            disconnected();
            return;
        }
        queuedBytes_ += bytes.size();
        queue_.push_back(std::move(bytes));
        if (queue_.size() == 1)
            write();
    }
    void write() {
        socket_.async_write(
            net::buffer(queue_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->disconnected();
                    return;
                }
                self->queuedBytes_ -= self->queue_.front().size();
                self->queue_.pop_front();
                if (!self->queue_.empty())
                    self->write();
                else if (self->closing_)
                    self->finishClose();
            });
    }
    void close(int code) {
        if (closing_ || dead_)
            return;
        closing_ = true;
        closeCode_ = code;
        session_.onTransportClosed(timeNow());
        if (queue_.empty())
            finishClose();
    }
    void finishClose() {
        socket_.async_close(
            ws::close_reason(static_cast<ws::close_code>(closeCode_)),
            [self = shared_from_this()](beast::error_code) {
                self->disconnected();
            });
    }
    void disconnected() {
        if (dead_)
            return;
        dead_ = true;
        session_.onTransportClosed(timeNow());
        // Keep ownership until the grace period ends, then the host destructor
        // closes the PTY and reaps even a HUP-ignoring child.
        cleanupAt_ = Clock::now() + shared_->config.process.terminationGrace;
        beast::error_code ignored;
        beast::get_lowest_layer(socket_).socket().close(ignored);
    }
    void schedule() {
        if (dead_ && Clock::now() >= cleanupAt_)
            return;
        auto delay = protocol::Time(10);
        if (!dead_) {
            const auto deadline = session_.nextDeadline();
            if (deadline)
                delay = std::min(
                    delay, std::max(protocol::Time(0), *deadline - timeNow()));
        }
        timer_.expires_after(delay);
        timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec)
                return;
            try {
                if (!self->dead_) {
                    if (!self->shared_->sessions.attachedValid(
                            self->generation_, false))
                        self->close(1008);
                    self->session_.tick(timeNow());
                }
                self->host_.pump(self->session_, timeNow());
            }
            catch (const std::exception &) {
                self->disconnected();
            }
            self->schedule();
        });
    }
    ws::stream<beast::tcp_stream> socket_;
    net::steady_timer timer_;
    std::shared_ptr<Shared> shared_;
    beast::flat_buffer buffer_{32768};
    http::request_parser<http::string_body> parser_;
    std::optional<http::response<http::string_body>> response_;
    PtySessionHost host_;
    protocol::ServerSession session_;
    std::deque<std::string> queue_;
    std::size_t queuedBytes_ = 0;
    std::string source_;
    bool loopback_ = false, authenticated_ = false, closing_ = false,
         dead_ = false;
    int closeCode_ = 1000;
    std::uint64_t generation_ = 0;
    Clock::time_point cleanupAt_{};
};
} // namespace
struct Server::Impl : std::enable_shared_from_this<Impl> {
    tcp::acceptor acceptor;
    std::shared_ptr<Shared> shared;
    bool started = false;
    Impl(net::io_context &io, ServerConfig config)
        : acceptor(io) {
        if (::geteuid() == 0)
            throw std::runtime_error("matterm-server refuses to run as root");
        const auto address = net::ip::make_address(config.address);
        if (config.address != "127.0.0.1" && !config.allowPlainHttp)
            throw std::invalid_argument(
                "Only 127.0.0.1 is allowed without --allow-plain-http");
        if (config.address != "127.0.0.1")
            std::clog << "WARNING: plain HTTP listening beyond 127.0.0.1\n";
        const auto &origin = config.allowedOrigin;
        const auto scheme = origin.find("://");
        if ((origin.rfind("http://", 0) != 0 &&
             origin.rfind("https://", 0) != 0) ||
            scheme == std::string::npos || origin.size() <= scheme + 3 ||
            origin.find_first_of("/\r\n\t @?#", scheme + 3) !=
                std::string::npos ||
            !config.maxConnections || !config.maxUnauthenticated)
            throw std::invalid_argument("Invalid server configuration/origin");
        shared = std::make_shared<Shared>(std::move(config));
        acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind({address, shared->config.port});
        acceptor.listen(16);
    }
    void accept() {
        acceptor.async_accept([self = shared_from_this()](beast::error_code ec,
                                                          tcp::socket socket) {
            if (!ec) {
                if (self->shared->connections <
                        self->shared->config.maxConnections &&
                    self->shared->unauthenticated <
                        self->shared->config.maxUnauthenticated) {
                    try {
                        std::make_shared<Connection>(std::move(socket),
                                                     self->shared)
                            ->start();
                    }
                    catch (const std::exception
                               &) { /* No secrets in diagnostics. */
                    }
                }
                // Otherwise the local socket destructor rejects it immediately.
            }
            if (self->acceptor.is_open())
                self->accept();
        });
    }
};
Server::Server(net::io_context &io, ServerConfig config)
    : impl_(std::make_shared<Impl>(io, std::move(config))) {}
Server::~Server() {
    beast::error_code ignored;
    impl_->acceptor.close(ignored);
}
void Server::start() {
    if (!impl_->started) {
        impl_->started = true;
        impl_->accept();
    }
}
unsigned short Server::port() const {
    return impl_->acceptor.local_endpoint().port();
}
} // namespace server
