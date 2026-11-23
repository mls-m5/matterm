#pragma once
// Shared fixture for tests that run a real matterm daemon in a thread and talk to its socket
// with a blocking client. Include it from one translation unit per test binary.
#include "control.h"
#include "daemon.h"
#include "framing.h"
#include "message.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
// How long a test waits for something that should happen. Generous on purpose: ctest -j runs the
// whole suite on a loaded machine; a success never waits for it, only a failure does.
constexpr int kPatience = 20000;

// A blocking test client speaking the daemon framing.
class DaemonClient {
public:
    explicit DaemonClient(const std::string &path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~DaemonClient() { close(); }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool connected() const { return fd_ >= 0; }
    bool sendRaw(const std::string &bytes) const {
        return ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL) == ssize_t(bytes.size());
    }
    bool sendFrame(const std::string &payload) const {
        std::string f;
        return mtd::encodeFrame(payload, f) && sendRaw(f);
    }
    template <class M>
    bool send(const M &m) const {
        std::string b;
        protocol::encode(protocol::ClientMessage{m}, b);
        return sendFrame(b);
    }
    bool sendControl(const mtd::ControlRequest &m) const {
        std::string b;
        return mtd::encode(m, b) && sendFrame(b);
    }
    // Next frame, or nullopt on EOF/timeout. `eof` tells which.
    std::optional<std::string> frame(int timeoutMs = kPatience) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            std::optional<std::string> got;
            if (!pending_.empty()) {
                got = std::move(pending_.front());
                pending_.erase(pending_.begin());
                return got;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0 || fd_ < 0) return std::nullopt;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, int(left.count())) <= 0) continue;
            char buf[4096];
            const auto n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) {
                eof = true;
                return std::nullopt;
            }
            if (!decoder_.feed(std::string_view(buf, std::size_t(n)),
                               [&](std::string_view f) { pending_.emplace_back(f); })) {
                eof = true;
                return std::nullopt;
            }
        }
    }
    std::optional<mtd::ControlReply> controlReply() {
        const auto f = frame();
        return f ? mtd::decodeReply(*f) : std::nullopt;
    }
    std::optional<protocol::ServerMessage> server(int timeoutMs = kPatience) {
        const auto f = frame(timeoutMs);
        if (!f) return std::nullopt;
        return protocol::decodeServer(*f).message;
    }
    // Reads server messages until the output so far contains `needle`.
    bool outputContains(const std::string &needle, int timeoutMs = kPatience) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            if (output.find(needle) != std::string::npos) return true;
            const auto m = server(200);
            if (!m) {
                if (eof) return false;
                continue;
            }
            if (const auto *o = std::get_if<protocol::Output>(&*m)) {
                output += o->data;
                send(protocol::Ack{o->offset + o->data.size()});
            }
        }
        return output.find(needle) != std::string::npos;
    }
    bool atEof(int timeoutMs = kPatience) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            frame(200);
            if (eof) return true;
        }
        return false;
    }
    std::string output;
    bool eof = false;

private:
    int fd_ = -1;
    mtd::FrameDecoder decoder_;
    std::vector<std::string> pending_;
};

struct Running {
    std::string dir;
    std::string path;
    std::unique_ptr<mtd::Daemon> d;
    std::thread thread;
    explicit Running(unsigned idle = 60, std::optional<uid_t> uid = std::nullopt, std::string existingDir = {},
                     std::function<void(mtd::DaemonConfig &)> tune = {}) {
        if (existingDir.empty()) {
            std::string t = (std::filesystem::temp_directory_path() / "mtdXXXXXX").string();
            dir = ::mkdtemp(t.data());
        }
        else {
            dir = existingDir;
        }
        path = dir + "/d.sock";
        auto config = mtd::defaultConfig();
        config.socketPath = path;
        config.privateDirectory = false;
        config.idleExitSeconds = idle;
        config.allowedUid = uid;
        config.process.arguments = {"/bin/sh"};
        if (tune) tune(config);
        d = std::make_unique<mtd::Daemon>(std::move(config));
        std::string error;
        started = d->start(error);
        startError = error;
        if (started) thread = std::thread([this] { d->run(); });
    }
    ~Running() {
        stop();
        if (!dir.empty() && ownsDir) std::filesystem::remove_all(dir);
    }
    void stop() {
        if (thread.joinable()) {
            d->stop();
            thread.join();
        }
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
    bool started = false;
    bool ownsDir = true;
    std::string startError;
};

protocol::Hello hello(const std::string &id = {}, std::uint64_t offset = 0) {
    protocol::Hello h;
    h.sessionId = id;
    h.resumeOffset = offset;
    h.features = protocol::featureReplay | protocol::featureDaemon;
    return h;
}

[[maybe_unused]] std::optional<protocol::Welcome> attach(
    DaemonClient &c, const std::string &id = {}, std::uint64_t offset = 0) {
    if (!c.send(hello(id, offset))) return std::nullopt;
    const auto m = c.server();
    if (!m) return std::nullopt;
    if (const auto *w = std::get_if<protocol::Welcome>(&*m)) return *w;
    return std::nullopt;
}
} // namespace


namespace {
// ControlHello and return a connected control client.
[[maybe_unused]] std::unique_ptr<DaemonClient> controlClient(
    const std::string &path) {
    auto c = std::make_unique<DaemonClient>(path);
    if (!c->sendControl(mtd::ControlHello{})) return nullptr;
    const auto w = c->controlReply();
    return w && std::holds_alternative<mtd::ControlWelcome>(*w) ? std::move(c) : nullptr;
}
[[maybe_unused]] std::unique_ptr<DaemonClient> exposedClient(const std::string &path) {
    auto c = std::make_unique<DaemonClient>(path);
    if (!c->sendControl(mtd::ExposedHello{})) return nullptr;
    const auto w = c->controlReply();
    return w && std::holds_alternative<mtd::ControlWelcome>(*w) ? std::move(c) : nullptr;
}
[[maybe_unused]] std::optional<std::string> createSession(
    DaemonClient &ctl, const std::string &cwd) {
    if (!ctl.sendControl(mtd::CreateSession{cwd, 100, 30})) return std::nullopt;
    const auto r = ctl.controlReply();
    if (r && std::holds_alternative<mtd::SessionCreated>(*r)) return std::get<mtd::SessionCreated>(*r).id;
    return std::nullopt;
}
[[maybe_unused]] std::optional<mtd::SessionInfo> info(DaemonClient &ctl, const std::string &id) {
    if (!ctl.sendControl(mtd::GetSessionInfo{id})) return std::nullopt;
    const auto r = ctl.controlReply();
    if (r && std::holds_alternative<mtd::SessionInfo>(*r)) return std::get<mtd::SessionInfo>(*r);
    return std::nullopt;
}
} // namespace

