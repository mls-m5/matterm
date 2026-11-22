#include <utility>

#include "localsessions.h"

#include "control.h"
#include "framing.h"

#include <boost/asio.hpp>
#include <array>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>
#include <deque>

namespace server {
namespace net = boost::asio;
using stream = net::local::stream_protocol;

struct DaemonConnection::Impl {
    stream::socket socket;
    mtd::FrameDecoder decoder;
    std::array<char, 8192> chunk{};
    std::deque<std::string> queue;
    OnFrame onFrame;
    OnClosed onClosed;
    bool connected = false, writing = false, closed = false, swallowed = false;
    std::weak_ptr<DaemonConnection> self;
    explicit Impl(net::any_io_executor ex) : socket(ex) {}
};

DaemonConnection::DaemonConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DaemonConnection::~DaemonConnection() = default;

std::shared_ptr<DaemonConnection> DaemonConnection::open(net::any_io_executor executor,
                                                         const std::string &path,
                                                         OnFrame onFrame,
                                                         OnClosed onClosed,
                                                         std::optional<unsigned> expectedPeerUid) {
    auto impl = std::make_unique<Impl>(executor);
    impl->onFrame = std::move(onFrame);
    impl->onClosed = std::move(onClosed);
    std::shared_ptr<DaemonConnection> c(new DaemonConnection(std::move(impl)));
    c->impl_->self = c;
    std::string hello, framed;
    mtd::encode(mtd::ControlRequest(mtd::ExposedHello{}), hello);
    mtd::encodeFrame(hello, framed);
    c->impl_->queue.push_back(std::move(framed));
    try {
        const uid_t expected = expectedPeerUid ? uid_t(*expectedPeerUid) : ::geteuid();
        c->impl_->socket.async_connect(stream::endpoint(path), [c, expected](const boost::system::error_code &error) {
            if (error) {
                c->close();
                return;
            }
            ucred cred{};
            socklen_t length = sizeof cred;
            if (::getsockopt(c->impl_->socket.native_handle(), SOL_SOCKET, SO_PEERCRED, &cred, &length) != 0 ||
                cred.uid != expected) {
                c->close(); // Not our daemon: send it nothing.
                return;
            }
            c->impl_->connected = true;
            c->pumpRead();
            c->pumpWrite();
        });
    }
    catch (const std::exception &) { // A path that cannot be an endpoint.
        net::post(executor, [c] { c->close(); });
    }
    return c;
}

void DaemonConnection::pumpRead() {
    auto &i = *impl_;
    if (i.closed) return;
    i.socket.async_read_some(net::buffer(i.chunk),
                             [c = shared_from_this()](const boost::system::error_code &ec, std::size_t n) {
                                 auto &k = *c->impl_;
                                 if (ec || k.closed) return c->close();
                                 // Hold the callback: onFrame may close the connection.
                                 const auto deliver = k.onFrame;
                                 const bool ok = k.decoder.feed(std::string_view(k.chunk.data(), n),
                                                                [&](std::string_view f) {
                                                                    if (k.closed) return;
                                                                    if (!k.swallowed) {
                                                                        k.swallowed = true;
                                                                        if (mtd::isControlType(std::uint8_t(f[0])))
                                                                            return;
                                                                    }
                                                                    if (deliver) deliver(std::string(f));
                                                                });
                                 if (!ok) return c->close();
                                 c->pumpRead();
                             });
}

void DaemonConnection::pumpWrite() {
    auto &i = *impl_;
    if (i.closed || !i.connected || i.writing || i.queue.empty()) return;
    i.writing = true;
    net::async_write(i.socket, net::buffer(i.queue.front()),
                     [c = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                         auto &k = *c->impl_;
                         k.writing = false;
                         if (ec) return c->close();
                         if (!k.queue.empty()) k.queue.pop_front();
                         c->pumpWrite();
                     });
}

void DaemonConnection::send(const std::string &payload) {
    auto &i = *impl_;
    if (i.closed) return;
    std::string framed;
    if (!mtd::encodeFrame(payload, framed) || i.queue.size() > 1024) { // Or the daemon stopped reading.
        close();
        return;
    }
    i.queue.push_back(std::move(framed));
    pumpWrite();
}

void DaemonConnection::close() {
    auto &i = *impl_;
    if (i.closed) return;
    i.closed = true;
    boost::system::error_code ignored;
    i.socket.close(ignored);
    auto done = std::move(i.onClosed);
    i.onClosed = nullptr;
    i.onFrame = nullptr;
    if (done) done();
}

bool DaemonConnection::isClosed() const { return impl_->closed; }

void listSharedSessions(net::any_io_executor executor,
                        const std::string &path,
                        std::chrono::milliseconds timeout,
                        std::function<void(std::optional<std::vector<LocalSessionInfo>>)> done,
                        std::optional<unsigned> expectedPeerUid) {
    struct State {
        std::function<void(std::optional<std::vector<LocalSessionInfo>>)> done;
        std::shared_ptr<DaemonConnection> connection;
        std::shared_ptr<net::steady_timer> timer;
        bool sent = false;
        void finish(std::optional<std::vector<LocalSessionInfo>> result) {
            auto d = std::move(done);
            done = nullptr;
            if (timer) timer->cancel();
            if (auto c = std::move(connection)) c->close();
            if (d) d(std::move(result));
        }
    };
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    state->timer = std::make_shared<net::steady_timer>(executor);
    state->timer->expires_after(timeout);
    state->timer->async_wait([state](const boost::system::error_code &ec) {
        if (!ec) state->finish(std::nullopt);
    });
    // The connection owns the callbacks, the callbacks own the state: broken in finish().
    state->connection = DaemonConnection::open(
        executor, path,
        [state](std::string frame) {
            const auto reply = mtd::decodeReply(frame);
            if (!reply) return state->finish(std::nullopt);
            if (const auto *list = std::get_if<mtd::SessionList>(&*reply)) {
                std::vector<LocalSessionInfo> out;
                for (const auto &s : list->sessions) out.push_back({s.id, s.attached, s.name});
                return state->finish(std::move(out));
            }
            state->finish(std::nullopt);
        },
        [state] { state->finish(std::nullopt); },
        expectedPeerUid);
    std::string request;
    mtd::encode(mtd::ControlRequest(mtd::ListSessions{}), request);
    state->connection->send(request);
}

} // namespace server
