#include "server.h"
#include "localsessions.h"
#include "proxysource.h"
#include "socketpath.h"
#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
struct StaticFile {
    std::string body;
    std::string type;
};
std::string contentType(const std::string &name) {
    static const std::map<std::string, std::string> types{
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".wasm", "application/wasm"},
        {".data", "application/octet-stream"},
        {".ttf", "font/ttf"},
        {".png", "image/png"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".txt", "text/plain; charset=utf-8"}};
    const auto found =
        types.find(std::filesystem::path(name).extension().string());
    return found == types.end() ? "application/octet-stream" : found->second;
}
// Reads the regular files directly inside `directory` (no recursion, no
// symlinks), so a request can only ever name something listed here.
std::map<std::string, StaticFile> loadStatic(const std::string &directory) {
    namespace fs = std::filesystem;
    std::map<std::string, StaticFile> files;
    if (directory.empty())
        return files;
    constexpr std::uintmax_t totalLimit = 64u << 20;
    std::uintmax_t total = 0;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.is_symlink() || !entry.is_regular_file())
            continue;
        const auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        total += entry.file_size();
        if (total > totalLimit)
            throw std::runtime_error("Static files exceed 64 MiB");
        std::ifstream in(entry.path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        files[name] = {std::move(body), contentType(name)};
    }
    if (!files.count("index.html"))
        throw std::runtime_error("Static directory has no index.html");
    return files;
}
class Connection;
struct Shared {
    ServerConfig config;
    // Browser connections bridged to a shared daemon session, by daemon session id.
    std::map<std::string, std::weak_ptr<Connection>> bridges;
    std::map<std::string, StaticFile> files;
    PasswordVerifier password;
    ProxySource proxy;
    RateLimiter limiter;
    SessionStore sessions;
    SessionRegistry registry;
    unsigned connections = 0, unauthenticated = 0;
    std::array<unsigned char, crypto_shorthash_KEYBYTES> auditKey{};
    std::string auditSource(const std::string &source) const {
        std::array<unsigned char, crypto_shorthash_BYTES> digest{};
        crypto_shorthash(digest.data(),
                         reinterpret_cast<const unsigned char *>(source.data()),
                         source.size(),
                         auditKey.data());
        std::array<char, crypto_shorthash_BYTES * 2 + 1> text{};
        sodium_bin2hex(text.data(), text.size(), digest.data(), digest.size());
        return text.data();
    }
    std::string daemonSocket() const {
        return config.daemonSocket.empty() ? mtd::newestDaemonSocket()
                                           : config.daemonSocket;
    }
    explicit Shared(ServerConfig cfg)
        : config(std::move(cfg))
        , files(loadStatic(config.staticDir))
        , password(config.passwordHash)
        , proxy(config.trustedProxy, config.trustedProxyHops)
        , limiter(config.rateLimit)
        , sessions(config.auth)
        , registry(config.registry, config.process, config.session) {
        randombytes_buf(auditKey.data(), auditKey.size());
    }
};
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(tcp::socket socket, std::shared_ptr<Shared> shared)
        : socket_(std::move(socket))
        , timer_(socket_.get_executor())
        , shared_(std::move(shared)) {
        const auto address = beast::get_lowest_layer(socket_)
                                 .socket()
                                 .remote_endpoint()
                                 .address();
        peer_ = address;
        source_ = sourceKey(address);
        loopback_ = peerIsLoopback(address);
        parser_.header_limit(8192);
        parser_.body_limit(512);
        // Count last: a throwing constructor never runs the destructor.
        ++shared_->connections;
        ++shared_->unauthenticated;
    }
    ~Connection() {
        --shared_->connections;
        if (!authenticated_)
            --shared_->unauthenticated;
    }
    void start() {
        beast::get_lowest_layer(socket_).expires_after(
            shared_->config.requestTimeout);
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
                  unsigned retry = 0,
                  const std::string &type = "text/html; charset=utf-8",
                  bool page = false) {
        response_.emplace(status, 11);
        response_->set(http::field::content_type, type);
        response_->set(http::field::cache_control, "no-store");
        // The client is same-origin scripts only (no inline script or style);
        // WebAssembly needs 'wasm-unsafe-eval'. Everything else is denied.
        response_->set(
            "Content-Security-Policy",
            page ? "default-src 'none'; script-src 'self' "
                   "'wasm-unsafe-eval'; style-src 'self'; connect-src 'self'; "
                   "img-src 'self'; font-src 'self'; frame-ancestors 'none'; "
                   "base-uri 'none'; form-action 'none'"
                 : "default-src 'none'; frame-ancestors 'none'; base-uri "
                   "'none'");
        response_->set("Referrer-Policy", "no-referrer");
        response_->set("X-Content-Type-Options", "nosniff");
        // No window reference to or from a hostile page, no cross-origin
        // embedding of any response.
        response_->set("Cross-Origin-Opener-Policy", "same-origin");
        response_->set("Cross-Origin-Resource-Policy", "same-origin");
        if (!cookie.empty())
            response_->set(http::field::set_cookie, cookie);
        if (retry)
            response_->set(http::field::retry_after, std::to_string(retry));
        response_->keep_alive(false);
        response_->body() = std::move(body);
        response_->prepare_payload();
        beast::get_lowest_layer(socket_).expires_after(
            std::chrono::seconds(30));
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
        if (request.method() == http::verb::get && !shared_->files.empty()) {
            // Exact names only; a query string is ignored, nothing is joined
            // to a path.
            std::string name(view(request.target()));
            name = name.substr(0, name.find('?'));
            name =
                name == "/" ? "index.html" : name.substr(name.empty() ? 0 : 1);
            const auto found = shared_->files.find(name);
            if (found != shared_->files.end()) {
                response(http::status::ok,
                         found->second.body,
                         {},
                         0,
                         found->second.type,
                         true);
                return;
            }
        }
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
        const auto forwarded = request.count("X-Forwarded-For");
        const auto client = shared_->proxy.resolve(
            peer_,
            forwarded ? std::optional<std::string_view>(
                            view(request["X-Forwarded-For"]))
                      : std::nullopt,
            forwarded > 1);
        if (!client) {
            response(http::status::bad_request, "Bad request\n");
            return;
        }
        source_ = sourceKey(*client);
        loopback_ = peerIsLoopback(*client);
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
                // NOLINTNEXTLINE(clang-analyzer-cplusplus.InnerPointer): false
                // positive, the string_view above is dead here
                sodium_memzero(password->data(), password->size());
            sodium_memzero(request.body().data(), request.body().size());
            request.body().clear();
            if (!matched || !password) {
                shared_->limiter.failure(source_);
                std::clog << "Authentication failed source="
                          << shared_->auditSource(source_) << '\n';
                response(http::status::unauthorized, "Login failed\n");
            }
            else {
                shared_->limiter.success(source_);
                std::clog << "Authentication succeeded source="
                          << shared_->auditSource(source_) << '\n';
                const auto token = shared_->sessions.issue();
                response(http::status::ok,
                         "Logged in\n",
                         "matterm_session=" + token +
                             "; HttpOnly; Secure; SameSite=Strict; Path=/");
            }
            return;
        }
        if (request.method() == http::verb::post &&
            (request.target() == "/logout" ||
             request.target() == "/sessions/close-all")) {
            const auto token =
                request.count(http::field::cookie) == 1
                    ? sessionCookie(view(request[http::field::cookie]))
                    : std::nullopt;
            if (!token || !shared_->sessions.valid(*token)) {
                response(http::status::unauthorized, "Unauthorized\n");
                return;
            }
            shared_->registry.closeAll(timeNow());
            if (request.target() == "/logout") {
                shared_->sessions.revoke();
                response(http::status::ok,
                         "Logged out\n",
                         "matterm_session=; Max-Age=0; HttpOnly; Secure; "
                         "SameSite=Strict; Path=/");
            }
            else
                response(http::status::ok, "Sessions closed\n");
            return;
        }
        if (request.method() == http::verb::post &&
            request.target() == "/local-sessions") {
            // POST so that browsers send an Origin header (checked above).
            const auto token =
                request.count(http::field::cookie) == 1
                    ? sessionCookie(view(request[http::field::cookie]))
                    : std::nullopt;
            if (!token || !shared_->sessions.valid(*token)) {
                response(http::status::unauthorized, "Unauthorized\n");
                return;
            }
            if (!shared_->config.exposeLocalSessions) {
                response(http::status::not_found, "Not found\n");
                return;
            }
            listLocalSessions();
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
            if (!token || !shared_->sessions.valid(*token)) {
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
                shared_->config.webSocketHandshakeTimeout,
                shared_->config.webSocketIdleTimeout,
                false});
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
                    self->parser_.get() = {};
                    self->helloDeadline_ =
                        Clock::now() +
                        self->shared_->config.session.helloTimeout;
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
                    if (self->attachment_ &&
                        (ec == ws::error::message_too_big ||
                         ec == ws::condition::protocol_violation))
                        self->shared_->registry.fail(*self->attachment_,
                                                     timeNow());
                    self->disconnected();
                    return;
                }
                if (!self->socket_.got_binary()) {
                    if (self->attachment_)
                        self->shared_->registry.fail(*self->attachment_,
                                                     timeNow());
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
                    if (self->bridge_) {
                        self->bridge_->send(bytes);
                    }
                    else if (self->attachment_) {
                        self->shared_->registry.onMessage(
                            *self->attachment_, bytes, timeNow());
                    }
                    else {
                        const auto decoded = protocol::decodeClient(bytes);
                        if (decoded && std::holds_alternative<protocol::Hello>(*decoded.message) &&
                            std::get<protocol::Hello>(*decoded.message).sessionId.rfind("local:", 0) == 0) {
                            self->startBridge(std::get<protocol::Hello>(*decoded.message));
                        }
                        else if (!decoded ||
                            !std::holds_alternative<protocol::Hello>(
                                *decoded.message)) {
                            self->protocolError(protocol::ErrorCode::BadMessage,
                                                "expected Hello");
                        }
                        else {
                            std::weak_ptr<Connection> weak = self;
                            SessionRegistry::Transport transport{
                                [weak](std::string data) {
                                    if (const auto s = weak.lock())
                                        s->send(std::move(data));
                                },
                                [weak](int code) {
                                    if (const auto s = weak.lock())
                                        s->close(code);
                                },
                                [weak](bool transfer) {
                                    const auto s = weak.lock();
                                    if (!s ||
                                        !s->shared_->sessions.claimGeneration(
                                            s->generation_, transfer))
                                        return false;
                                    s->lease_ = true;
                                    s->authenticated_ = true;
                                    --s->shared_->unauthenticated;
                                    return true;
                                },
                                [weak] {
                                    if (const auto s = weak.lock())
                                        s->releaseLease();
                                },
                                self->generation_};
                            self->attachment_ = self->shared_->registry.attach(
                                std::get<protocol::Hello>(*decoded.message),
                                std::move(transport),
                                timeNow());
                        }
                    }
                }
                catch (const std::exception &) {
                    if (self->attachment_)
                        self->shared_->registry.fail(*self->attachment_,
                                                     timeNow());
                    self->close(1011);
                }
                if (!self->closing_ && !self->dead_)
                    self->read();
            });
    }
    static bool plainId(const std::string &id) {
        return !id.empty() && id.size() <= protocol::maxSessionIdSize &&
               std::all_of(id.begin(), id.end(), [](unsigned char c) {
                   return std::isalnum(c) || c == '_' || c == '-';
               });
    }
    // POST /local-sessions: the shared sessions of the local daemon, as JSON.
    void listLocalSessions() {
        const auto path = shared_->daemonSocket();
        listSharedSessions(
            socket_.get_executor(),
            path,
            std::chrono::milliseconds(2000),
            [self = shared_from_this()](
                std::optional<std::vector<LocalSessionInfo>> sessions) {
                if (!sessions) {
                    self->response(http::status::bad_gateway,
                                   "Local daemon unavailable\n");
                    return;
                }
                std::string body = "{\"sessions\":[";
                bool first = true;
                for (const auto &s : *sessions) {
                    if (!plainId(s.id))
                        continue;
                    body += (first ? "" : ",");
                    body += "{\"id\":\"" + s.id + "\",\"attached\":" +
                            (s.attached ? "true" : "false") + ",\"name\":\"";
                    for (const char c : s.name) { // No control characters: the daemon refuses them.
                        if (c == '"' || c == '\\') body += '\\';
                        body += c;
                    }
                    body += "\"}";
                    first = false;
                }
                body += "]}\n";
                self->response(http::status::ok,
                               std::move(body),
                               {},
                               0,
                               "application/json");
            },
            shared_->config.daemonPeerUid);
    }
    // The browser asked for "local:<id>": take over a shared session of the local daemon.
    // The daemon decides what is visible; this connection only forwards frames both ways.
    void startBridge(protocol::Hello hello) {
        const auto refuse = [this] {
            protocolError(protocol::ErrorCode::ResumeUnsupported,
                          "session unavailable");
        };
        hello.sessionId.erase(0, 6);
        if (!shared_->config.exposeLocalSessions || !plainId(hello.sessionId)) {
            refuse();
            return;
        }
        const auto id = hello.sessionId;
        std::shared_ptr<Connection> previous;
        if (const auto found = shared_->bridges.find(id);
            found != shared_->bridges.end())
            previous = found->second.lock();
        const bool transfer = previous && previous->lease_;
        if (!shared_->sessions.claimGeneration(generation_, transfer)) {
            protocolError(protocol::ErrorCode::LimitExceeded, "upgrade limit");
            return;
        }
        lease_ = true;
        if (!authenticated_) {
            authenticated_ = true;
            --shared_->unauthenticated;
        }
        if (previous)
            previous->protocolError(protocol::ErrorCode::Replaced,
                                    "session taken over");
        shared_->bridges[id] = shared_from_this();
        bridgeId_ = id;
        // Who took which terminal, never what was on the screen.
        std::clog << "Local session take-over attempt session=" << id
                  << " source=" << shared_->auditSource(source_) << '\n';
        std::weak_ptr<Connection> weak = shared_from_this();
        bridge_ = DaemonConnection::open(
            socket_.get_executor(),
            shared_->daemonSocket(),
            [weak](std::string frame) {
                if (const auto s = weak.lock())
                    s->fromDaemon(std::move(frame));
            },
            [weak] {
                if (const auto s = weak.lock())
                    s->bridgeClosed();
            },
            shared_->config.daemonPeerUid);
        // The daemon gets as long as a Hello gets to answer.
        helloDeadline_ = Clock::now() + shared_->config.session.helloTimeout;
        std::string bytes;
        protocol::encode(protocol::ClientMessage{hello}, bytes);
        bridge_->send(bytes);
    }
    void fromDaemon(std::string frame) {
        bridgeAnswered_ = true;
        if (!frame.empty() && std::uint8_t(frame[0]) == 0x85) {
            // The daemon explains why it ends the session: keep the matching close code.
            const auto decoded = protocol::decodeServer(frame);
            if (decoded && std::holds_alternative<protocol::Error>(*decoded.message))
                bridgeCloseCode_ = protocol::closeCodeFor(
                    std::get<protocol::Error>(*decoded.message).code);
        }
        send(std::move(frame));
    }
    void bridgeClosed() {
        if (closing_ || dead_)
            return;
        if (!bridgeAnswered_)
            protocolError(protocol::ErrorCode::Internal,
                          "local sessions unavailable");
        else
            close(bridgeCloseCode_);
    }
    void releaseLease() {
        if (lease_) {
            shared_->sessions.release(generation_);
            lease_ = false;
        }
    }
    void protocolError(protocol::ErrorCode code, const char *reason) {
        std::string bytes;
        protocol::encode(protocol::ServerMessage{protocol::Error{code, reason}},
                         bytes);
        send(std::move(bytes));
        close(protocol::closeCodeFor(code));
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
        closeDeadline_ =
            Clock::now() + shared_->config.webSocketHandshakeTimeout;
        releaseLease();
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
        if (attachment_)
            shared_->registry.detach(*attachment_, timeNow());
        if (bridge_) {
            const auto bridge = std::move(bridge_);
            bridge_.reset();
            bridge->close();
        }
        if (!bridgeId_.empty()) {
            const auto found = shared_->bridges.find(bridgeId_);
            if (found != shared_->bridges.end()) {
                const auto owner = found->second.lock();
                if (!owner || owner.get() == this)
                    shared_->bridges.erase(found);
            }
        }
        releaseLease();
        timer_.cancel();
        beast::error_code ignored;
        beast::get_lowest_layer(socket_).socket().close(ignored);
    }
    void schedule() {
        if (dead_)
            return;
        timer_.expires_after(protocol::Time(10));
        timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec)
                return;
            if (self->closing_) {
                if (Clock::now() >= self->closeDeadline_)
                    self->disconnected();
            }
            else if (!self->shared_->sessions.attachedValid(self->generation_,
                                                            false)) {
                self->close(1008); // Cookie rotation/expiry detaches, never
                                   // transfers old rights.
            }
            else if (!self->attachment_ && !self->bridge_ &&
                     Clock::now() >= self->helloDeadline_) {
                self->protocolError(protocol::ErrorCode::Timeout,
                                    "expected Hello");
            }
            else if (self->bridge_ && !self->bridgeAnswered_ &&
                     Clock::now() >= self->helloDeadline_) {
                self->protocolError(protocol::ErrorCode::Timeout,
                                    "local session did not answer");
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
    std::optional<SessionRegistry::Attachment> attachment_;
    std::shared_ptr<DaemonConnection> bridge_;
    std::string bridgeId_;
    bool bridgeAnswered_ = false;
    int bridgeCloseCode_ = 1000;
    std::deque<std::string> queue_;
    std::size_t queuedBytes_ = 0;
    std::string source_;
    net::ip::address peer_;
    bool loopback_ = false, authenticated_ = false, closing_ = false,
         dead_ = false, lease_ = false;
    int closeCode_ = 1000;
    std::uint64_t generation_ = 0;
    Clock::time_point helloDeadline_{}, closeDeadline_{};
};
} // namespace
struct Server::Impl : std::enable_shared_from_this<Impl> {
    tcp::acceptor acceptor;
    net::steady_timer pumpTimer;
    std::shared_ptr<Shared> shared;
    bool started = false;
    Impl(net::io_context &io, ServerConfig config)
        : acceptor(io)
        , pumpTimer(io) {
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
        if (config.exposeLocalSessions) {
            if (!address.is_loopback() && !config.allowRemoteListen)
                throw std::invalid_argument(
                    "--expose-local-sessions beyond loopback needs "
                    "--allow-remote-listen");
            std::clog << "matterm-server: shared sessions of the local daemon ("
                      << (config.daemonSocket.empty()
                              ? mtd::newestDaemonSocket()
                              : config.daemonSocket)
                      << ") are offered to logged-in browsers\n";
            if (!address.is_loopback())
                std::clog << "WARNING: local terminals are reachable from the "
                             "network (--allow-remote-listen); anyone with the "
                             "password gets a shell as you\n";
        }
        if (config.trustedProxy == "any")
            std::clog << "WARNING: trusted_proxy=any requires network "
                         "isolation; only the proxy may reach this port\n";
        shared = std::make_shared<Shared>(std::move(config));
        acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind({address, shared->config.port});
        acceptor.listen(16);
    }
    void pump() {
        auto delay = protocol::Time(10);
        if (const auto deadline = shared->registry.nextDeadline())
            delay = std::min(
                delay, std::max(protocol::Time(0), *deadline - timeNow()));
        pumpTimer.expires_after(delay);
        pumpTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (ec || !self->acceptor.is_open())
                return;
            try {
                self->shared->registry.tick(timeNow());
            }
            catch (const std::exception &) {
                self->shared->registry.closeAll(timeNow());
            }
            self->pump();
        });
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
    impl_->pumpTimer.cancel();
}
void Server::start() {
    if (!impl_->started) {
        impl_->started = true;
        impl_->accept();
        impl_->pump();
    }
}
unsigned short Server::port() const {
    return impl_->acceptor.local_endpoint().port();
}
} // namespace server
