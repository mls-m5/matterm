#include "daemon.h"

#include "control.h"
#include "framing.h"
#include "message.h"
#include "paneprobe.h"
#include "procinfo.h"

#include <utility>

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <csignal>
#include <cstring>
#include <thread>
#include <sys/un.h>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mtd {
namespace net = boost::asio;
using stream = net::local::stream_protocol;
using Clock = std::chrono::steady_clock;

namespace {
protocol::Time timeNow() {
    return std::chrono::duration_cast<protocol::Time>(Clock::now().time_since_epoch());
}
constexpr std::size_t kMaxQueueBytes = 524288;
constexpr std::size_t kMaxQueueMessages = 1024;
} // namespace

DaemonConfig defaultConfig() {
    DaemonConfig c;
    c.socketPath = defaultSocketPath();
    c.registry.persistentSessions = true;
    c.registry.replayBufferBytes = 4u << 20;
    c.registry.replayTotalBytes = 256u << 20;
    c.registry.maxSessions = 64;
    c.registry.maxDetachedSessions = 64;
    c.registry.detachedTimeout = protocol::Time(0);
    c.registry.extraFeatures = protocol::featureDaemon;
    c.process.environmentAllowList = {"HOME",   "USER",  "LOGNAME", "PATH",   "SHELL",           "LANG",
                                      "LC_ALL", "LC_CTYPE", "TERM", "COLORTERM", "SSH_AUTH_SOCK",
                                      "XDG_RUNTIME_DIR", "DISPLAY", "WAYLAND_DISPLAY"};
    const char *shell = std::getenv("SHELL");
    c.process.arguments = {shell && *shell ? shell : "/bin/sh"};
    return c;
}

struct Daemon::Impl : std::enable_shared_from_this<Impl> {
    class Connection;
    DaemonConfig config;
    net::io_context ioc;
    std::optional<stream::acceptor> acceptor;
    std::optional<net::steady_timer> pump;
    std::optional<net::signal_set> signals;
    std::unique_ptr<server::SessionRegistry> registry;
    std::vector<std::weak_ptr<Connection>> connections;
    unsigned clients = 0;
    int lockFd = -1;
    bool socketBound = false;
    bool stopping = false;
    bool shutdownPending = false; // A confirmed Shutdown was accepted: nothing new may start.
    std::atomic<ExitReason> exitReason{ExitReason::None};
    Clock::time_point started = Clock::now();
    std::optional<Clock::time_point> idleSince;

    explicit Impl(DaemonConfig c) : config(std::move(c)) {
        if (!config.allowedUid) config.allowedUid = geteuid();
    }
    ~Impl() {
        if (socketBound) ::unlink(config.socketPath.c_str());
        if (lockFd >= 0) ::close(lockFd);
    }

    bool start(std::string &error);
    void accept();
    void schedulePump();
    void finish(ExitReason why);
    // A session stopped being shared: whoever the web server attached to it is dropped.
    void cutExposed(const std::string &id);
    std::uint32_t liveSessions() const { return std::uint32_t(registry->list().size()); }
};

class Daemon::Impl::Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(Impl &daemon, stream::socket socket)
        : d_(daemon), socket_(std::move(socket)), timer_(socket_.get_executor()) {}
    ~Connection() {
        if (counted_) --d_.clients;
    }
    void begin() {
        counted_ = true;
        ++d_.clients;
        d_.idleSince.reset();
        armHelloTimer();
        read();
    }

private:
    enum class Mode { Unknown, Exposed, Terminal, Control };

    void armHelloTimer() {
        timer_.expires_after(std::chrono::milliseconds(d_.config.helloTimeoutMs));
        timer_.async_wait([self = shared_from_this()](const boost::system::error_code &ec) {
            // Also an exposed connection that never follows up with its Hello or a list request.
            if (!ec && (self->mode_ == Mode::Unknown || (self->mode_ == Mode::Exposed && !self->attachment_)))
                self->kill();
        });
    }
    void read() {
        socket_.async_read_some(net::buffer(chunk_), [self = shared_from_this()](auto ec, std::size_t n) {
            if (ec) {
                self->disconnected();
                return;
            }
            bool ok = true;
            try {
                ok = self->decoder_.feed(std::string_view(self->chunk_.data(), n),
                                         [&](std::string_view frame) {
                                             if (!self->dead_ && !self->closing_) self->onFrame(frame);
                                         });
            }
            catch (const std::exception &) {
                ok = false;
            }
            if (!ok) {
                // Framing violated: not a client of ours. Fail the session if one is attached.
                if (self->attachment_) self->d_.registry->fail(*self->attachment_, timeNow());
                self->kill();
                return;
            }
            if (!self->dead_ && !self->closing_) self->read();
        });
    }
    void onFrame(std::string_view frame) {
        if (mode_ == Mode::Terminal) {
            d_.registry->onMessage(*attachment_, frame, timeNow());
            return;
        }
        if (mode_ == Mode::Control) {
            control(frame);
            return;
        }
        if (isControlType(std::uint8_t(frame[0]))) {
            const auto request = decodeRequest(frame);
            if (mode_ == Mode::Exposed) {
                // Only the shared list is on offer here.
                if (request && std::holds_alternative<ListSessions>(*request)) {
                    handle(ListSessions{});
                    return;
                }
                controlError(ControlErrorCode::BadMessage, "not available to an exposed connection");
                closeAfterFlush();
                return;
            }
            const bool exposed = request && std::holds_alternative<ExposedHello>(*request);
            if (!request || (!exposed && !std::holds_alternative<ControlHello>(*request))) {
                controlError(ControlErrorCode::BadMessage, "expected ControlHello");
                closeAfterFlush();
                return;
            }
            mode_ = exposed ? Mode::Exposed : Mode::Control;
            ControlWelcome w;
            w.pid = std::uint32_t(getpid());
            w.features = protocol::featureDaemon;
            w.build = d_.config.build;
            reply(w);
            return;
        }
        const auto decoded = protocol::decodeClient(frame);
        if (!decoded || !std::holds_alternative<protocol::Hello>(*decoded.message)) {
            protocolError(protocol::ErrorCode::BadMessage, "expected Hello");
            return;
        }
        const auto &hello = std::get<protocol::Hello>(*decoded.message);
        if (mode_ == Mode::Exposed && !isShared(hello.sessionId)) {
            // Unknown, private and gone look the same from outside.
            protocolError(protocol::ErrorCode::ResumeUnsupported, "session unavailable");
            return;
        }
        if (d_.shutdownPending) {
            protocolError(protocol::ErrorCode::ShuttingDown, "shutting down");
            return;
        }
        std::weak_ptr<Connection> weak = shared_from_this();
        server::SessionRegistry::Transport transport;
        transport.send = [weak](std::string data) {
            if (const auto s = weak.lock()) s->sendRaw(data);
        };
        transport.close = [weak](int) {
            if (const auto s = weak.lock()) s->closeAfterFlush();
        };
        exposedAttach_ = mode_ == Mode::Exposed;
        mode_ = Mode::Terminal; // Set first: attach may send Welcome or Error immediately.
        attachment_ = d_.registry->attach(hello, std::move(transport), timeNow());
        if (!attachment_) closeAfterFlush();
    }
    bool isShared(const std::string &id) const {
        if (id.empty()) return false; // An exposed connection cannot create sessions.
        for (const auto &i : d_.registry->list()) {
            if (i.id == id) return i.shared;
        }
        return false;
    }
    void control(std::string_view frame) {
        const auto request = decodeRequest(frame);
        if (!request) {
            controlError(ControlErrorCode::BadMessage, "bad control message");
            closeAfterFlush();
            return;
        }
        std::visit([this](const auto &m) { handle(m); }, *request);
    }
    void handle(const ControlHello &) {
        controlError(ControlErrorCode::BadMessage, "duplicate ControlHello");
        closeAfterFlush();
    }
    void handle(const ExposedHello &) { handle(ControlHello{}); }
    void handle(const ListSessions &) {
        SessionList list;
        for (const auto &i : d_.registry->list()) {
            if (mode_ == Mode::Exposed && !i.shared) continue;
            if (list.sessions.size() >= kMaxListed) break;
            list.sessions.push_back({i.id, i.attached, i.shared, i.outputEnd});
        }
        reply(list);
    }
    void handle(const SetShared &m) {
        if (!d_.registry->setShared(m.id, m.shared)) {
            controlError(ControlErrorCode::UnknownSession, "no such session");
            return;
        }
        if (!m.shared) d_.cutExposed(m.id);
        handle(ListSessions{});
    }
    void handle(const CreateSession &m) {
        if (d_.shutdownPending) {
            controlError(ControlErrorCode::Busy, "shutting down");
            return;
        }
        std::string cwd;
        struct stat st{};
        if (frontend::plainPath(m.cwd) && ::stat(m.cwd.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) cwd = m.cwd;
        const auto id = d_.registry->create(cwd, m.columns, m.rows, timeNow());
        if (!id) {
            controlError(ControlErrorCode::StartFailed, "cannot start a session");
            return;
        }
        reply(SessionCreated{*id});
    }
    void handle(const GetSessionInfo &m) {
        const auto ids = d_.registry->processIds(m.id);
        if (!ids) {
            controlError(ControlErrorCode::UnknownSession, "no such session");
            return;
        }
        const frontend::LinuxProcSource source;
        const auto state = frontend::probePane(source, ids->shell, ids->foreground);
        SessionInfo info;
        info.id = m.id;
        info.cwd = state.cwd;
        info.busy = state.busy;
        info.command = state.command;
        reply(info);
    }
    void handle(const GetStatus &) {
        Status s;
        s.pid = std::uint32_t(getpid());
        for (const auto &i : d_.registry->list()) {
            ++s.sessions;
            if (i.attached) ++s.attached;
            if (i.shared) ++s.shared;
        }
        s.clients = d_.clients;
        s.uptimeMs = std::uint64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - d_.started).count());
        s.idleExitSeconds = d_.config.idleExitSeconds;
        reply(s);
    }
    void handle(const Shutdown &m) {
        const auto live = d_.liveSessions();
        if (m.expectedSessions != live) {
            reply(ShutdownReply{false, live});
            return;
        }
        d_.shutdownPending = true; // The owner confirmed this many: no session may slip in unconfirmed.
        reply(ShutdownReply{true, live});
        // Let the reply reach the client, then end everything.
        onFlushed_ = [&daemon = d_] { daemon.finish(ExitReason::Shutdown); };
        flushIfIdle();
    }

    template <class M>
    void reply(const M &message) {
        std::string bytes;
        if (!encode(ControlReply(message), bytes)) return kill();
        sendRaw(bytes);
    }
    void controlError(ControlErrorCode code, const char *reason) { reply(ControlError{code, reason}); }
    void protocolError(protocol::ErrorCode code, const char *reason) {
        std::string bytes;
        protocol::encode(protocol::ServerMessage{protocol::Error{code, reason}}, bytes);
        sendRaw(bytes);
        closeAfterFlush();
    }

    // `payload` is one message as a WebSocket would carry it.
    void sendRaw(const std::string &payload) {
        if (dead_ || closing_) return;
        if (payload.size() > kMaxQueueBytes - queuedBytes_ || queue_.size() >= kMaxQueueMessages) {
            kill(); // The client does not read: same policy as the WebSocket server.
            return;
        }
        std::string framed;
        if (!encodeFrame(payload, framed)) return kill();
        queuedBytes_ += framed.size();
        queue_.push_back(std::move(framed));
        if (!writing_) write();
    }
    void write() {
        if (queue_.empty()) {
            writing_ = false;
            flushIfIdle();
            return;
        }
        writing_ = true;
        net::async_write(socket_, net::buffer(queue_.front()), [self = shared_from_this()](auto ec, std::size_t) {
            if (ec) {
                self->disconnected();
                return;
            }
            self->queuedBytes_ -= self->queue_.front().size();
            self->queue_.pop_front();
            self->write();
        });
    }
    void flushIfIdle() {
        if (writing_ || !queue_.empty()) return;
        if (auto f = std::move(onFlushed_)) {
            onFlushed_ = nullptr;
            f();
        }
        if (closing_) kill();
    }
    // The session this connection shows to the web server, if it is one.
    const std::string *exposedSession() const {
        return exposedAttach_ && attachment_ ? &attachment_->id : nullptr;
    }
    void cut(protocol::ErrorCode code, const char *reason) { protocolError(code, reason); }
    void closeAfterFlush() {
        if (dead_) return;
        closing_ = true;
        flushIfIdle();
    }
    void kill() {
        if (dead_) return;
        dead_ = true;
        boost::system::error_code ignored;
        socket_.shutdown(stream::socket::shutdown_both, ignored);
        socket_.close(ignored);
        timer_.cancel();
        detachSession();
    }
    void disconnected() {
        if (dead_) return;
        dead_ = true;
        boost::system::error_code ignored;
        socket_.close(ignored);
        timer_.cancel();
        detachSession();
    }
    void detachSession() {
        if (attachment_) {
            auto a = std::move(*attachment_);
            attachment_.reset();
            d_.registry->detach(a, timeNow());
        }
        if (counted_) {
            counted_ = false;
            --d_.clients;
        }
        if (d_.clients == 0 && !d_.idleSince) d_.idleSince = Clock::now();
    }

    Impl &d_;
    stream::socket socket_;
    net::steady_timer timer_;
    FrameDecoder decoder_;
    std::array<char, 8192> chunk_{};
    std::deque<std::string> queue_;
    std::size_t queuedBytes_ = 0;
    std::optional<server::SessionRegistry::Attachment> attachment_;
    std::function<void()> onFlushed_;
    Mode mode_ = Mode::Unknown;
    bool counted_ = false;
    bool exposedAttach_ = false;
    bool writing_ = false;
    bool closing_ = false;
    bool dead_ = false;

    friend struct Daemon::Impl;
};

bool Daemon::Impl::start(std::string &error) {
    const auto &path = config.socketPath;
    sockaddr_un probe{};
    if (path.empty() || path.size() >= sizeof(probe.sun_path)) {
        error = "socket path empty or too long";
        return false;
    }
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
    if (config.privateDirectory) {
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot create " + dir + ": " + std::strerror(errno);
            return false;
        }
        // lstat: a symlink here was planted by whoever could write the parent directory.
        struct stat st{};
        if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 077) != 0) {
            error = dir + " must be a real directory (not a symlink) owned by you with mode 0700";
            return false;
        }
    }
    // O_NOFOLLOW: a symlink planted as the lock file must not make us truncate its target.
    lockFd = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFd < 0) {
        error = std::string("cannot open lock file: ") + std::strerror(errno);
        return false;
    }
    struct stat lockStat{};
    if (::fstat(lockFd, &lockStat) != 0 || !S_ISREG(lockStat.st_mode) || lockStat.st_uid != geteuid()) {
        error = path + ".lock is not a regular file of yours";
        ::close(lockFd);
        lockFd = -1;
        return false;
    }
    if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        error = "another daemon already owns " + path;
        ::close(lockFd);
        lockFd = -1;
        return false;
    }
    const auto pid = std::to_string(getpid()) + "\n";
    if (::ftruncate(lockFd, 0) == 0) {
        [[maybe_unused]] const auto written = ::write(lockFd, pid.data(), pid.size());
    }
    // We hold the lock, so a socket of ours left here is stale. Anything else is not ours to remove.
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
            error = path + " exists and is not a socket of yours";
            return false;
        }
        ::unlink(path.c_str());
    }
    struct UmaskGuard {
        mode_t old;
        UmaskGuard() : old(::umask(077)) {}
        ~UmaskGuard() { ::umask(old); }
        UmaskGuard(const UmaskGuard &) = delete;
        UmaskGuard &operator=(const UmaskGuard &) = delete;
    };
    try {
        {
            const UmaskGuard guard; // The socket is born 0600, never briefly wider.
            acceptor.emplace(ioc, stream::endpoint(path));
        }
        socketBound = true;
        [[maybe_unused]] const auto chmodResult = ::chmod(path.c_str(), 0600);
    }
    catch (const std::exception &e) {
        error = std::string("cannot listen on ") + path + ": " + e.what();
        return false;
    }
    registry = std::make_unique<server::SessionRegistry>(config.registry, config.process, protocol::SessionConfig{});
    pump.emplace(ioc);
    (void)::signal(SIGPIPE, SIG_IGN);
    signals.emplace(ioc, SIGINT, SIGTERM);
    signals->async_wait([this](const boost::system::error_code &ec, int) {
        if (!ec) finish(ExitReason::Stopped);
    });
    idleSince = Clock::now();
    accept();
    schedulePump();
    return true;
}

void Daemon::Impl::accept() {
    acceptor->async_accept([this](const boost::system::error_code &ec, stream::socket socket) {
        if (stopping) return;
        if (!ec) {
            ucred cred{};
            socklen_t len = sizeof cred;
            const bool trusted =
                ::getsockopt(socket.native_handle(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
                cred.uid == *config.allowedUid;
            if (trusted && clients < config.maxClients) {
                auto c = std::make_shared<Connection>(*this, std::move(socket));
                connections.push_back(c);
                c->begin();
            }
            // Anyone else is closed without a word: the socket is not theirs to talk to.
            std::erase_if(connections, [](const auto &w) { return w.expired(); });
        }
        accept();
    });
}

void Daemon::Impl::schedulePump() {
    // 10 ms while sessions exist (PTY output), slow when there is nothing to do.
    const bool busy = registry->size() > 0;
    pump->expires_after(std::chrono::milliseconds(busy ? 10 : 200));
    pump->async_wait([this](const boost::system::error_code &ec) {
        if (ec || stopping) return;
        try {
            registry->tick(timeNow());
        }
        catch (const std::exception &) {
        }
        if (registry->size() == 0 && clients == 0) {
            if (!idleSince) idleSince = Clock::now();
            if (config.idleExitSeconds > 0 &&
                Clock::now() - *idleSince >= std::chrono::seconds(config.idleExitSeconds)) {
                finish(ExitReason::Idle);
                return;
            }
        }
        else {
            idleSince.reset();
        }
        schedulePump();
    });
}

void Daemon::Impl::cutExposed(const std::string &id) {
    for (auto &w : connections) {
        if (const auto c = w.lock()) {
            const auto *s = c->exposedSession();
            if (s && *s == id) c->cut(protocol::ErrorCode::Replaced, "sharing stopped");
        }
    }
}

void Daemon::Impl::finish(ExitReason why) {
    if (stopping) return;
    stopping = true;
    exitReason = why;
    boost::system::error_code ignored;
    acceptor->close(ignored);
    if (socketBound) {
        ::unlink(config.socketPath.c_str()); // Before the lock goes, so a successor never loses its socket to us.
        socketBound = false;
    }
    pump->cancel();
    for (auto &w : connections) {
        if (const auto c = w.lock()) c->kill();
    }
    connections.clear();
    registry->closeAll(timeNow());
    // closeAll starts the termination; give the registry a few ticks to reap before stopping.
    for (int i = 0; i < 300 && registry->size() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        registry->tick(timeNow());
    }
    ioc.stop();
}

Daemon::Daemon(DaemonConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Daemon::~Daemon() = default;
bool Daemon::start(std::string &error) { return impl_->start(error); }
void Daemon::run() {
    impl_->ioc.run();
}
void Daemon::stop() {
    net::post(impl_->ioc, [this] { impl_->finish(ExitReason::Stopped); });
}
ExitReason Daemon::reason() const { return impl_->exitReason; }

} // namespace mtd
