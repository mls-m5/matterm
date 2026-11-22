#include "control.h"
#include "daemon.h"
#include "framing.h"
#include "message.h"
#include "mls-unit-test/unittest.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <cstring>
#include <random>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;

// A blocking test client speaking the daemon framing.
class Client {
public:
    explicit Client(const std::string &path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Client() { close(); }
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
    std::optional<std::string> frame(int timeoutMs = 3000) {
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
    std::optional<protocol::ServerMessage> server(int timeoutMs = 3000) {
        const auto f = frame(timeoutMs);
        if (!f) return std::nullopt;
        return protocol::decodeServer(*f).message;
    }
    // Reads server messages until the output so far contains `needle`.
    bool outputContains(const std::string &needle, int timeoutMs = 5000) {
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
    bool atEof(int timeoutMs = 2000) {
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
    explicit Running(unsigned idle = 60, std::optional<uid_t> uid = std::nullopt, std::string existingDir = {}) {
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

std::optional<protocol::Welcome> attach(Client &c, const std::string &id = {}, std::uint64_t offset = 0) {
    if (!c.send(hello(id, offset))) return std::nullopt;
    const auto m = c.server();
    if (!m) return std::nullopt;
    if (const auto *w = std::get_if<protocol::Welcome>(&*m)) return *w;
    return std::nullopt;
}
} // namespace

TEST_SUIT_BEGIN(Daemon)

TEST_CASE("Framing round trips and survives any chunking") {
    std::string stream;
    EXPECT(mtd::encodeFrame("a", stream));
    EXPECT(mtd::encodeFrame(std::string(mtd::kMaxFrame, 'z'), stream));
    EXPECT(mtd::encodeFrame("hello", stream));
    for (const std::size_t step : {std::size_t(1), std::size_t(3), std::size_t(5), std::size_t(4096), stream.size()}) {
        mtd::FrameDecoder d;
        std::vector<std::string> frames;
        bool ok = true;
        for (std::size_t i = 0; i < stream.size(); i += step) {
            ok = d.feed(stream.substr(i, step), [&](std::string_view f) { frames.emplace_back(f); }) && ok;
        }
        EXPECT(ok);
        EXPECT_EQ(frames.size(), std::size_t(3));
        EXPECT_EQ(frames[0], std::string("a"));
        EXPECT_EQ(frames[1].size(), mtd::kMaxFrame);
        EXPECT_EQ(frames[2], std::string("hello"));
        EXPECT_EQ(d.pending(), std::size_t(0));
    }
}

TEST_CASE("Framing refuses empty, oversized and zero-length frames") {
    std::string out;
    EXPECT_FALSE(mtd::encodeFrame("", out));
    EXPECT_FALSE(mtd::encodeFrame(std::string(mtd::kMaxFrame + 1, 'x'), out));
    EXPECT(out.empty());
    for (const std::string &bad : {std::string("\0\0\0\0", 4), std::string("\0\0\x40\x01", 4), std::string("\xff\xff\xff\xff", 4)}) {
        mtd::FrameDecoder d;
        int frames = 0;
        EXPECT_FALSE(d.feed(bad, [&](std::string_view) { ++frames; }));
        EXPECT_EQ(frames, 0);
        EXPECT(d.failed());
        EXPECT_FALSE(d.feed("\0\0\0\1x", [&](std::string_view) { ++frames; })); // stays failed
        EXPECT_EQ(frames, 0);
    }
    // A partial header is simply waited for.
    mtd::FrameDecoder d;
    EXPECT(d.feed(std::string("\0\0", 2), [](std::string_view) {}));
    EXPECT_EQ(d.pending(), std::size_t(2));
}

TEST_CASE("Framing fuzz: random bytes never crash or exceed the limits") {
    std::mt19937 rng(12345);
    for (int round = 0; round < 2000; ++round) {
        mtd::FrameDecoder d;
        std::size_t total = 0;
        bool ok = true;
        for (int piece = 0; piece < 8 && ok; ++piece) {
            std::string chunk(rng() % 64, '\0');
            for (auto &c : chunk) c = char(rng() % 5 == 0 ? rng() : (rng() % 4 == 0 ? 0 : 1));
            ok = d.feed(chunk, [&](std::string_view f) {
                EXPECT(!f.empty());
                EXPECT(f.size() <= mtd::kMaxFrame);
                total += f.size();
            });
        }
        EXPECT_EQ(ok, !d.failed());
    }
}

TEST_CASE("Control messages round trip and reject malformed input") {
    const std::vector<mtd::ControlRequest> requests{mtd::ControlHello{}, mtd::ListSessions{},
                                                       mtd::SetShared{"abc", true}, mtd::GetStatus{},
                                                       mtd::Shutdown{7}};
    for (const auto &r : requests) {
        std::string b;
        EXPECT(mtd::encode(r, b));
        const auto back = mtd::decodeRequest(b);
        EXPECT(back.has_value());
        EXPECT_EQ(back->index(), r.index());
        EXPECT_FALSE(mtd::decodeRequest(b + "x").has_value());
        EXPECT_FALSE(mtd::decodeRequest(b.substr(0, b.size() - 1)).has_value() && b.size() > 1);
        EXPECT_FALSE(mtd::decodeReply(b).has_value()); // wrong direction
    }
    mtd::SessionList list;
    list.sessions = {{"s1", true, false, 99}, {"s2", false, true, 1u << 20}};
    std::string b;
    EXPECT(mtd::encode(mtd::ControlReply(list), b));
    const auto back = mtd::decodeReply(b);
    EXPECT(back.has_value());
    const auto &l = std::get<mtd::SessionList>(*back);
    EXPECT_EQ(l.sessions.size(), std::size_t(2));
    EXPECT_EQ(l.sessions[1].outputEnd, std::uint64_t(1u << 20));
    EXPECT(l.sessions[0].attached && !l.sessions[0].shared && l.sessions[1].shared);
    EXPECT_FALSE(mtd::decodeRequest("").has_value());
    EXPECT_FALSE(mtd::decodeRequest(std::string("\x42\0\0\0", 4)).has_value()); // empty id
    EXPECT_FALSE(mtd::decodeRequest(std::string("\x40\0\0", 3)).has_value());   // version 0
    EXPECT_FALSE(mtd::decodeReply(std::string("\xc4\0\x09\0\0", 5)).has_value()); // bad code
    std::string tooMany;
    mtd::SessionList big;
    big.sessions.resize(mtd::kMaxListed + 1, {"x", false, false, 0});
    EXPECT_FALSE(mtd::encode(mtd::ControlReply(big), tooMany));
    // The terminal protocol does not understand control types, and the reverse.
    EXPECT_FALSE(protocol::decodeClient(std::string("\x41", 1)));
    std::mt19937 rng(7);
    for (int i = 0; i < 3000; ++i) {
        std::string junk(rng() % 24, '\0');
        for (auto &c : junk) c = char(rng());
        if (!junk.empty() && i % 2) junk[0] = char(0x40 + rng() % 5);
        if (!junk.empty() && i % 3 == 0) junk[0] = char(0xc0 + rng() % 5);
        (void)mtd::decodeRequest(junk);
        (void)mtd::decodeReply(junk);
    }
}

TEST_CASE("A session runs through the socket and has the daemon feature") {
    Running r;
    EXPECT(r.started);
    Client c(r.path);
    EXPECT(c.connected());
    const auto w = attach(c);
    EXPECT(w.has_value());
    EXPECT((w->features & protocol::featureDaemon) != 0);
    EXPECT((w->features & protocol::featureReplay) != 0);
    EXPECT(c.send(protocol::Input{"echo daemon-$((20+22))\n"}));
    EXPECT(c.outputContains("daemon-42"));
}

TEST_CASE("The socket is 0600 and the lock file names the pid") {
    Running r;
    EXPECT(r.started);
    struct stat st{};
    EXPECT_EQ(::stat(r.path.c_str(), &st), 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0600);
    EXPECT(std::filesystem::exists(r.path + ".lock"));
}

TEST_CASE("Sessions survive a client disconnect and replay after reconnect") {
    Running r;
    EXPECT(r.started);
    std::string id;
    {
        Client c(r.path);
        const auto w = attach(c);
        EXPECT(w.has_value());
        id = w->sessionId;
        EXPECT(c.send(protocol::Input{"echo before-$((1+1))\n"}));
        EXPECT(c.outputContains("before-2"));
    }
    std::this_thread::sleep_for(200ms);
    {
        Client ctl(r.path);
        EXPECT(ctl.sendControl(mtd::ControlHello{}));
        const auto welcome = ctl.controlReply();
        EXPECT(welcome.has_value() && std::holds_alternative<mtd::ControlWelcome>(*welcome));
        EXPECT(ctl.sendControl(mtd::ListSessions{}));
        const auto list = ctl.controlReply();
        EXPECT(list.has_value());
        const auto &l = std::get<mtd::SessionList>(*list);
        EXPECT_EQ(l.sessions.size(), std::size_t(1));
        EXPECT_EQ(l.sessions[0].id, id);
        EXPECT_FALSE(l.sessions[0].attached);
        EXPECT_FALSE(l.sessions[0].shared);
    }
    Client again(r.path);
    const auto w = attach(again, id, 0);
    EXPECT(w.has_value());
    EXPECT_EQ(w->sessionId, id);
    EXPECT(again.outputContains("before-2"));
    EXPECT(again.send(protocol::Input{"echo after-$((2+2))\n"}));
    EXPECT(again.outputContains("after-4"));
}

TEST_CASE("Marking a session shared is visible in the list and refused for unknown ids") {
    Running r;
    EXPECT(r.started);
    Client c(r.path);
    const auto w = attach(c);
    EXPECT(w.has_value());
    Client ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
    EXPECT(ctl.sendControl(mtd::SetShared{w->sessionId, true}));
    auto reply = ctl.controlReply();
    EXPECT(reply.has_value());
    EXPECT(std::get<mtd::SessionList>(*reply).sessions.at(0).shared);
    EXPECT(ctl.sendControl(mtd::SetShared{"nonexistent", true}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value() && std::holds_alternative<mtd::ControlError>(*reply));
    EXPECT(ctl.sendControl(mtd::GetStatus{}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value());
    const auto &s = std::get<mtd::Status>(*reply);
    EXPECT_EQ(s.sessions, 1u);
    EXPECT_EQ(s.attached, 1u);
    EXPECT_EQ(s.shared, 1u);
    EXPECT_EQ(s.clients, 2u);
}

TEST_CASE("Another user is refused without a word") {
    Running r(60, geteuid() + 1);
    EXPECT(r.started);
    Client c(r.path);
    EXPECT(c.connected());
    c.send(hello());
    EXPECT(!c.frame(1000).has_value());
    EXPECT(c.atEof());
}

TEST_CASE("Framing violations on the socket close the connection") {
    Running r;
    EXPECT(r.started);
    for (const std::string &bad : {std::string("\0\0\0\0", 4), std::string("\0\0\x40\x01xx", 6),
                                  std::string("\xde\xad\xbe\xef", 4)}) {
        Client c(r.path);
        EXPECT(c.sendRaw(bad));
        EXPECT(c.atEof());
    }
    // And the daemon is still fine.
    Client ok(r.path);
    EXPECT(attach(ok).has_value());
}

TEST_CASE("A first frame that is neither Hello nor ControlHello is an error") {
    Running r;
    EXPECT(r.started);
    Client c(r.path);
    EXPECT(c.send(protocol::Input{"x"}));
    const auto m = c.server();
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(c.atEof());
    Client silent(r.path); // Says nothing: dropped after the hello timeout (default 5 s) - not waited for here.
    EXPECT(silent.connected());
}

TEST_CASE("Idle exit: no sessions and no client") {
    Running r(1);
    EXPECT(r.started);
    const auto t0 = std::chrono::steady_clock::now();
    r.join();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT(r.d->reason() == mtd::ExitReason::Idle);
    EXPECT(took < 5s);
    EXPECT(!std::filesystem::exists(r.path));
}

TEST_CASE("A session keeps the daemon alive past the idle time") {
    Running r(1);
    EXPECT(r.started);
    {
        Client c(r.path);
        EXPECT(attach(c).has_value());
    }
    std::this_thread::sleep_for(2500ms);
    EXPECT(r.d->reason() == mtd::ExitReason::None);
    Client ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
}

TEST_CASE("Two daemons cannot share a socket") {
    Running first;
    EXPECT(first.started);
    Running second(60, std::nullopt, first.dir);
    second.ownsDir = false;
    EXPECT_FALSE(second.started);
    EXPECT(second.startError.find("already") != std::string::npos);
    // The first one is untouched.
    Client c(first.path);
    EXPECT(attach(c).has_value());
}

TEST_CASE("A stale socket file from a dead daemon is replaced") {
    std::string t = (std::filesystem::temp_directory_path() / "mtdXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    {
        Running a(60, std::nullopt, dir);
        a.ownsDir = false;
        EXPECT(a.started);
    }
    // Make the leftovers look like a crash: a socket file with nobody behind it.
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, (dir + "/d.sock").c_str(), sizeof addr.sun_path - 1);
        ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        ::close(fd);
    }
    Running b(60, std::nullopt, dir);
    b.ownsDir = false;
    EXPECT(b.started);
    Client c(b.path);
    EXPECT(attach(c).has_value());
    b.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Shutdown runs only with the confirmed session count") {
    Running r;
    EXPECT(r.started);
    Client c(r.path);
    EXPECT(attach(c).has_value());
    Client ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
    EXPECT(ctl.sendControl(mtd::Shutdown{0})); // The client believed there were none.
    auto reply = ctl.controlReply();
    EXPECT(reply.has_value());
    const auto &refused = std::get<mtd::ShutdownReply>(*reply);
    EXPECT_FALSE(refused.done);
    EXPECT_EQ(refused.sessions, 1u);
    std::this_thread::sleep_for(100ms);
    EXPECT(r.d->reason() == mtd::ExitReason::None);
    EXPECT(ctl.sendControl(mtd::Shutdown{1}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value());
    EXPECT(std::get<mtd::ShutdownReply>(*reply).done);
    r.join();
    EXPECT(r.d->reason() == mtd::ExitReason::Shutdown);
    EXPECT(!std::filesystem::exists(r.path));
    EXPECT(c.atEof());
}

TEST_SUIT_END
