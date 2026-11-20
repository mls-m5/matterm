#include "chatserver.h"

#include "proxysource.h"
#include "ratelimiter.h"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <deque>
#include <istream>
#include <map>
#include <optional>

namespace chatd {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;

std::string_view view(boost::beast::string_view s) {
    return {s.data(), s.size()};
}

constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr auto kIdleTimeout = std::chrono::seconds(90);
constexpr auto kCloseGrace = std::chrono::seconds(3);

} // namespace

struct ChatServer::Impl : std::enable_shared_from_this<ChatServer::Impl> {
    class Session;
    class WsSession;
    class TcpSession;

    Impl(net::io_context &ioContext, Options o)
        : io(ioContext)
        , options(std::move(o))
        , room(options.room)
        , proxy(options.trustProxy)
        , timer(ioContext)
        , wsAcceptor(ioContext)
        , tcpAcceptor(ioContext)
        , started(Clock::now()) {}

    chat::Millis now() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - started)
            .count();
    }

    void apply(const chat::Actions &actions);
    void reschedule();
    void onOpen(const std::shared_ptr<Session> &session,
                const std::string &source);
    void onLine(chat::ConnId id, std::string_view frame);
    void onBadFrame(chat::ConnId id);
    void onClosed(chat::ConnId id);
    void backlog(chat::ConnId id, std::size_t bytes);
    void acceptWs();
    void acceptTcp();

    net::io_context &io;
    Options options;
    chat::ChatRoom room;
    server::ProxySource proxy;
    net::steady_timer timer;
    tcp::acceptor wsAcceptor, tcpAcceptor;
    Clock::time_point started;
    std::map<chat::ConnId, std::shared_ptr<Session>> sessions;
    std::size_t pending = 0;
    chat::ConnId nextId = 1;
    bool stopped = false;
};

// One connection, whatever its transport.
class ChatServer::Impl::Session : public std::enable_shared_from_this<Session> {
public:
    Session(Impl &impl, chat::ConnId id)
        : impl_(impl)
        , id_(id) {}
    virtual ~Session() = default;
    virtual void start() = 0;
    virtual void write() = 0;
    virtual void shutdown() = 0; // Politely, when nothing is being written.
    virtual void abort() = 0;    // At once.

    chat::ConnId id() const {
        return id_;
    }
    void enqueue(std::string line) {
        if (closing_) {
            return;
        }
        queued_ += line.size();
        out_.push_back(std::move(line));
        if (!writing_) {
            write();
        }
        impl_.backlog(id_, queued_);
    }
    // After what is queued has been written (or after a grace time).
    void close() {
        if (closing_) {
            return;
        }
        closing_ = true;
        if (!writing_ && out_.empty()) {
            shutdown();
        }
    }

protected:
    void wrote(std::size_t bytes) {
        writing_ = false;
        queued_ -= std::min(queued_, bytes);
        if (!out_.empty()) {
            write();
        }
        else if (closing_) {
            shutdown();
        }
    }
    Impl &impl_;
    chat::ConnId id_;
    std::deque<std::string> out_;
    std::size_t queued_ = 0;
    bool writing_ = false;
    bool closing_ = false;
    bool open_ = false; // The room knows this connection.
    bool gone_ = false;
    friend struct ChatServer::Impl;
};

// WebSocket text frames, after an HTTP upgrade that must carry an allowed
// Origin.
class ChatServer::Impl::WsSession final : public Session {
public:
    WsSession(Impl &impl, chat::ConnId id, tcp::socket socket)
        : Session(impl, id)
        , ws_(std::move(socket)) {
        peer_ =
            beast::get_lowest_layer(ws_).socket().remote_endpoint().address();
    }
    void start() override {
        beast::get_lowest_layer(ws_).expires_after(kHandshakeTimeout);
        parser_.body_limit(0);
        parser_.header_limit(8 * 1024);
        http::async_read(
            ws_.next_layer(),
            rbuf_,
            parser_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                static_cast<WsSession &>(*self).requested(ec);
            });
    }
    void write() override {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(
            net::buffer(out_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                auto &s = static_cast<WsSession &>(*self);
                const auto bytes = s.out_.front().size();
                s.out_.pop_front();
                if (ec) {
                    s.finish();
                    return;
                }
                s.wrote(bytes);
            });
    }
    void shutdown() override {
        if (gone_ || closeStarted_) {
            return;
        }
        closeStarted_ = true;
        ws_.async_close(ws::close_code::normal,
                        [self = shared_from_this()](beast::error_code) {
                            static_cast<WsSession &>(*self).finish();
                        });
    }
    void abort() override {
        finish();
    }

private:
    void reject(http::status status, const char *text) {
        auto reply =
            std::make_shared<http::response<http::string_body>>(status, 11);
        reply->set(http::field::content_type, "text/plain");
        reply->body() = text;
        reply->keep_alive(false);
        reply->prepare_payload();
        http::async_write(
            ws_.next_layer(),
            *reply,
            [self = shared_from_this(), reply](beast::error_code, std::size_t) {
                static_cast<WsSession &>(*self).finish();
            });
    }
    void requested(beast::error_code ec) {
        if (ec) {
            finish();
            return;
        }
        const auto &request = parser_.get();
        if (!ws::is_upgrade(request) || request.method() != http::verb::get ||
            request.target() != "/") {
            reject(http::status::not_found, "Not found\n");
            return;
        }
        // Origin: exactly one, and one of ours. It is a browser-enforced
        // header, not authentication: it stops other websites, nothing more.
        bool allowed = request.count(http::field::origin) == 1;
        if (allowed) {
            const auto origin = view(request[http::field::origin]);
            allowed = std::find(impl_.options.origins.begin(),
                                impl_.options.origins.end(),
                                origin) != impl_.options.origins.end();
        }
        if (!allowed) {
            reject(http::status::forbidden, "Forbidden\n");
            return;
        }
        const auto forwarded = request.count("X-Forwarded-For");
        const auto client = impl_.proxy.resolve(
            peer_,
            forwarded ? std::optional<std::string_view>(
                            view(request["X-Forwarded-For"]))
                      : std::nullopt,
            forwarded > 1);
        if (!client || rbuf_.size() != 0) {
            reject(http::status::bad_request, "Bad request\n");
            return;
        }
        source_ = server::sourceKey(*client);
        ws_.read_message_max(chat::kMaxFrameBytes);
        ws::permessage_deflate compression;
        compression.server_enable = false;
        compression.client_enable = false;
        ws_.set_option(compression);
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(
            ws::stream_base::timeout{kHandshakeTimeout, kIdleTimeout, true});
        ws_.async_accept(request,
                         [self = shared_from_this()](beast::error_code e) {
                             auto &s = static_cast<WsSession &>(*self);
                             if (e) {
                                 s.finish();
                                 return;
                             }
                             s.impl_.onOpen(self, s.source_);
                             s.read();
                         });
    }
    void read() {
        if (gone_) {
            return;
        }
        rbuf_.consume(rbuf_.size());
        ws_.async_read(
            rbuf_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                auto &s = static_cast<WsSession &>(*self);
                if (s.gone_) {
                    return;
                }
                if (ec == ws::error::message_too_big) {
                    s.impl_.onBadFrame(s.id_);
                    return;
                }
                if (ec) {
                    s.finish();
                    return;
                }
                if (!s.ws_.got_text()) {
                    s.impl_.onBadFrame(s.id_);
                    return;
                }
                const std::string frame =
                    beast::buffers_to_string(s.rbuf_.data());
                s.impl_.onLine(s.id_, frame);
                s.read();
            });
    }
    void finish() {
        if (gone_) {
            return;
        }
        gone_ = true;
        beast::error_code ignored;
        beast::get_lowest_layer(ws_).socket().close(ignored);
        impl_.onClosed(id_);
    }

    ws::stream<beast::tcp_stream> ws_;
    beast::flat_buffer rbuf_;
    http::request_parser<http::empty_body> parser_;
    net::ip::address peer_;
    std::string source_;
    bool closeStarted_ = false;
};

// Plain TCP lines, for local tests with nc or telnet.
class ChatServer::Impl::TcpSession final : public Session {
public:
    TcpSession(Impl &impl, chat::ConnId id, tcp::socket socket)
        : Session(impl, id)
        , socket_(std::move(socket))
        , rbuf_(chat::kMaxFrameBytes + 2) {
        source_ = server::sourceKey(socket_.remote_endpoint().address());
    }
    void start() override {
        impl_.onOpen(shared_from_this(), source_);
        read();
    }
    void write() override {
        writing_ = true;
        out_.front() += '\n';
        net::async_write(
            socket_,
            net::buffer(out_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                auto &s = static_cast<TcpSession &>(*self);
                const auto bytes = s.out_.front().size() - 1;
                s.out_.pop_front();
                if (ec) {
                    s.finish();
                    return;
                }
                s.wrote(bytes);
            });
    }
    void shutdown() override {
        abort();
    }
    void abort() override {
        if (gone_) {
            return;
        }
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        finish();
    }

private:
    void read() {
        if (gone_) {
            return;
        }
        net::async_read_until(
            socket_,
            rbuf_,
            '\n',
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                auto &s = static_cast<TcpSession &>(*self);
                if (s.gone_) {
                    return;
                }
                if (ec == net::error::not_found) {
                    s.impl_.onBadFrame(s.id_); // A line that is too long.
                    return;
                }
                if (ec) {
                    s.finish();
                    return;
                }
                std::istream stream(&s.rbuf_);
                std::string frame;
                std::getline(stream, frame);
                if (!frame.empty() && frame.back() == '\r') {
                    frame.pop_back();
                }
                if (!frame
                         .empty()) { // A bare Enter in telnet is not a mistake.
                    s.impl_.onLine(s.id_, frame);
                }
                s.read();
            });
    }
    void finish() {
        if (gone_) {
            return;
        }
        gone_ = true;
        beast::error_code ignored;
        socket_.close(ignored);
        impl_.onClosed(id_);
    }

    tcp::socket socket_;
    net::streambuf rbuf_;
    std::string source_;
};

void ChatServer::Impl::apply(const chat::Actions &actions) {
    for (const auto &a : actions) {
        const auto it = sessions.find(a.conn);
        if (it == sessions.end()) {
            continue;
        }
        const auto session = it->second; // Keep it alive while we act on it.
        if (a.close) {
            session->close();
            // A stuck writer must not keep the slot: close after a grace time.
            auto grace = std::make_shared<net::steady_timer>(io, kCloseGrace);
            grace->async_wait(
                [session, grace](beast::error_code) { session->shutdown(); });
        }
        else {
            session->enqueue(a.line);
        }
    }
}

void ChatServer::Impl::reschedule() {
    if (stopped) {
        return;
    }
    // At least once a second, for the room's housekeeping.
    auto next = now() + 1000;
    if (const auto d = room.nextDeadline()) {
        next = std::min(next, std::max(*d, now()));
    }
    timer.expires_at(started + std::chrono::milliseconds(next));
    timer.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec || self->stopped) {
            return;
        }
        self->apply(self->room.tick(self->now()));
        self->reschedule();
    });
}

void ChatServer::Impl::onOpen(const std::shared_ptr<Session> &session,
                              const std::string &source) {
    if (pending > 0) {
        --pending;
    }
    session->open_ = true;
    apply(room.connected(session->id(), source, now()));
    reschedule();
}

void ChatServer::Impl::onLine(chat::ConnId id, std::string_view frame) {
    apply(room.line(id, frame, now()));
    reschedule();
}

void ChatServer::Impl::onBadFrame(chat::ConnId id) {
    apply(room.badFrame(id, now()));
    const auto it = sessions.find(id);
    if (it != sessions.end()) {
        it->second->close();
    }
    reschedule();
}

void ChatServer::Impl::backlog(chat::ConnId id, std::size_t bytes) {
    apply(room.backlog(id, bytes, now()));
}

void ChatServer::Impl::onClosed(chat::ConnId id) {
    const auto it = sessions.find(id);
    if (it == sessions.end()) {
        return;
    }
    const bool wasOpen = it->second->open_;
    if (!wasOpen && pending > 0) {
        --pending;
    }
    sessions.erase(it);
    if (wasOpen) {
        apply(room.closed(id, now()));
        reschedule();
    }
}

void ChatServer::Impl::acceptWs() {
    wsAcceptor.async_accept(
        [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (self->stopped) {
                return;
            }
            if (!ec) {
                if (self->pending >= self->options.maxPending) {
                    beast::error_code ignored;
                    socket.close(ignored);
                }
                else {
                    ++self->pending;
                    const auto id = self->nextId++;
                    try {
                        auto session = std::make_shared<WsSession>(
                            *self, id, std::move(socket));
                        self->sessions[id] = session;
                        session->start();
                    }
                    catch (const std::exception &) {
                        --self->pending;
                    }
                }
            }
            self->acceptWs();
        });
}

void ChatServer::Impl::acceptTcp() {
    tcpAcceptor.async_accept(
        [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (self->stopped) {
                return;
            }
            if (!ec) {
                if (self->pending >= self->options.maxPending) {
                    beast::error_code ignored;
                    socket.close(ignored);
                }
                else {
                    ++self->pending;
                    const auto id = self->nextId++;
                    try {
                        auto session = std::make_shared<TcpSession>(
                            *self, id, std::move(socket));
                        self->sessions[id] = session;
                        session->start();
                    }
                    catch (const std::exception &) {
                        --self->pending;
                    }
                }
            }
            self->acceptTcp();
        });
}

ChatServer::ChatServer(net::io_context &io, Options options) {
    if (options.wsEnabled && options.origins.empty()) {
        throw std::invalid_argument(
            "WebSocket needs at least one allowed origin (--origin)");
    }
    for (const auto &o : options.origins) {
        const auto scheme = o.find("://");
        if ((o.rfind("http://", 0) != 0 && o.rfind("https://", 0) != 0) ||
            o.size() <= scheme + 3 ||
            o.find_first_of("/\r\n\t @?#", scheme + 3) != std::string::npos) {
            throw std::invalid_argument(
                "An origin looks like https://host[:port]: " + o);
        }
    }
    impl_ = std::make_shared<Impl>(io, std::move(options));
    auto &o = impl_->options;
    auto open = [&](tcp::acceptor &acceptor,
                    const std::string &host,
                    std::uint16_t port) {
        beast::error_code ec;
        const auto address = net::ip::make_address(host, ec);
        if (ec) {
            throw std::invalid_argument("Not an IP address: " + host);
        }
        const tcp::endpoint endpoint(address, port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(64);
    };
    if (o.tcpEnabled) {
        beast::error_code ec;
        const auto address = net::ip::make_address(o.tcpHost, ec);
        if (ec || !server::peerIsLoopback(address)) {
            throw std::invalid_argument(
                "The plain TCP test listener is for loopback addresses only");
        }
        open(impl_->tcpAcceptor, o.tcpHost, o.tcpPort);
    }
    if (o.wsEnabled) {
        open(impl_->wsAcceptor, o.wsHost, o.wsPort);
    }
}

ChatServer::~ChatServer() {
    stop();
}

void ChatServer::start() {
    if (impl_->options.tcpEnabled) {
        impl_->acceptTcp();
    }
    if (impl_->options.wsEnabled) {
        impl_->acceptWs();
    }
    impl_->reschedule();
}

void ChatServer::stop() {
    if (!impl_ || impl_->stopped) {
        return;
    }
    impl_->stopped = true;
    beast::error_code ignored;
    impl_->wsAcceptor.close(ignored);
    impl_->tcpAcceptor.close(ignored);
    impl_->timer.cancel();
    const auto all = impl_->sessions;
    for (const auto &entry : all) {
        entry.second->abort();
    }
}

std::uint16_t ChatServer::wsPort() const {
    return impl_->wsAcceptor.is_open()
               ? impl_->wsAcceptor.local_endpoint().port()
               : 0;
}

std::uint16_t ChatServer::tcpPort() const {
    return impl_->tcpAcceptor.is_open()
               ? impl_->tcpAcceptor.local_endpoint().port()
               : 0;
}

std::string ChatServer::console(std::string_view command) {
    std::string reply;
    impl_->apply(impl_->room.admin(command, impl_->now(), reply));
    impl_->reschedule();
    return reply;
}

chat::ChatRoom::Stats ChatServer::stats() const {
    return impl_->room.stats();
}

} // namespace chatd
