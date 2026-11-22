#include "clientsession.h"
#include "message.h"
#include "mls-unit-test/unittest.h"
#include "unixchannel.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <variant>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
// One end is the channel under test, the other a raw descriptor the test drives.
struct Pair {
    int peer = -1;
    std::unique_ptr<UnixSocketChannel> channel;
    std::vector<std::string> messages;
    std::vector<int> closes;
    int opens = 0;
    Pair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            std::abort();
        }
        peer = fds[0];
        ::fcntl(peer, F_SETFL, ::fcntl(peer, F_GETFL, 0) | O_NONBLOCK);
        channel = std::make_unique<UnixSocketChannel>(fds[1]);
        MessageChannel::Events events;
        events.opened = [this] { ++opens; };
        events.message = [this](std::string_view m) { messages.emplace_back(m); };
        events.closed = [this](int code) { closes.push_back(code); };
        channel->setEvents(std::move(events));
    }
    ~Pair() {
        if (peer >= 0) {
            ::close(peer);
        }
    }
    void write(const std::string &bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto n = ::write(peer, bytes.data() + done, bytes.size() - done);
            if (n <= 0) {
                break;
            }
            done += std::size_t(n);
        }
    }
    std::string read() {
        std::string out;
        char buffer[4096];
        for (;;) {
            const auto n = ::read(peer, buffer, sizeof buffer);
            if (n <= 0) {
                break;
            }
            out.append(buffer, std::size_t(n));
        }
        return out;
    }
    void closePeer() {
        ::close(peer);
        peer = -1;
    }
};

std::string frame(const std::string &payload) {
    const auto n = std::uint32_t(payload.size());
    std::string out;
    out.push_back(char(n >> 24));
    out.push_back(char(n >> 16));
    out.push_back(char(n >> 8));
    out.push_back(char(n));
    return out + payload;
}
} // namespace

TEST_SUIT_BEGIN(UnixChannel)

TEST_CASE("opened comes with the first poll, once") {
    Pair p;
    EXPECT_EQ(p.opens, 0);
    p.channel->poll();
    p.channel->poll();
    EXPECT_EQ(p.opens, 1);
    EXPECT(p.channel->isOpen());
}

TEST_CASE("send frames messages with a 4-byte big-endian length") {
    Pair p;
    p.channel->poll();
    EXPECT(p.channel->send("hello"));
    EXPECT(p.channel->send(std::string(300, 'x')));
    const auto raw = p.read();
    EXPECT_EQ(raw, frame("hello") + frame(std::string(300, 'x')));
    EXPECT(raw.substr(0, 4) == std::string("\0\0\0\5", 4));
    EXPECT(raw.substr(5 + 4, 4) == std::string("\0\0\x01\x2c", 4)); // 300
}

TEST_CASE("send refuses empty, oversize and closed") {
    Pair p;
    EXPECT(!p.channel->send(""));
    EXPECT(!p.channel->send(std::string(UnixSocketChannel::kMaxMessage + 1, 'a')));
    EXPECT(p.channel->send(std::string(UnixSocketChannel::kMaxMessage, 'a')));
    EXPECT(p.channel->isOpen()); // A refused message does not hurt the connection.
    p.channel->close(1000);
    EXPECT(!p.channel->send("late"));
}

TEST_CASE("received messages keep their boundaries however the bytes arrive") {
    Pair p;
    p.channel->poll();
    const auto bytes = frame("one") + frame("two") + frame(std::string(16384, 'z'));
    p.write(bytes.substr(0, 2)); // Split inside the length.
    p.channel->poll();
    EXPECT(p.messages.empty());
    p.write(bytes.substr(2, 8)); // Into the first and second message.
    p.channel->poll();
    EXPECT_EQ(p.messages.size(), std::size_t(1));
    p.write(bytes.substr(10));
    for (int i = 0; i < 4; ++i) {
        p.channel->poll();
    }
    EXPECT_EQ(p.messages.size(), std::size_t(3));
    EXPECT_EQ(p.messages[0], std::string("one"));
    EXPECT_EQ(p.messages[1], std::string("two"));
    EXPECT_EQ(p.messages[2], std::string(16384, 'z'));
    // Byte by byte.
    Pair q;
    q.channel->poll();
    const auto two = frame("ab") + frame("c");
    for (const char c : two) {
        q.write(std::string(1, c));
        q.channel->poll();
    }
    EXPECT_EQ(q.messages.size(), std::size_t(2));
    EXPECT(q.closes.empty());
}

TEST_CASE("a zero or oversize length closes the connection with 1002") {
    for (const std::string &bad : {std::string("\0\0\0\0", 4),
                                  std::string("\0\0\x40\x01", 4),
                                  std::string("\xff\xff\xff\xff", 4)}) {
        Pair p;
        p.channel->poll();
        p.write(frame("fine") + bad + frame("never"));
        p.channel->poll();
        EXPECT_EQ(p.messages.size(), std::size_t(1)); // Only what came before.
        EXPECT_EQ(p.closes.size(), std::size_t(1));
        EXPECT_EQ(p.closes[0], 1002);
        EXPECT(!p.channel->isOpen());
        p.channel->poll();
        EXPECT_EQ(p.closes.size(), std::size_t(1)); // Once.
    }
}

TEST_CASE("the peer going away: 1000 between messages, 1006 inside one") {
    Pair clean;
    clean.channel->poll();
    clean.write(frame("bye"));
    clean.closePeer();
    clean.channel->poll();
    EXPECT_EQ(clean.messages.size(), std::size_t(1));
    EXPECT_EQ(clean.closes.size(), std::size_t(1));
    EXPECT_EQ(clean.closes[0], 1000);
    Pair cut;
    cut.channel->poll();
    cut.write(frame("complete") + frame("truncated").substr(0, 6));
    cut.closePeer();
    cut.channel->poll();
    EXPECT_EQ(cut.messages.size(), std::size_t(1));
    EXPECT_EQ(cut.closes.size(), std::size_t(1));
    EXPECT_EQ(cut.closes[0], 1006);
    Pair vanished;
    vanished.channel->poll();
    vanished.closePeer();
    vanished.channel->poll();
    EXPECT_EQ(vanished.closes.size(), std::size_t(1));
}

TEST_CASE("close(code) reports that code once, from poll, and flushes what is queued") {
    Pair p;
    p.channel->poll();
    p.channel->send("last words");
    p.channel->close(1011);
    EXPECT(p.closes.empty()); // Not inside close().
    EXPECT(!p.channel->isOpen());
    p.channel->close(1000); // Again: nothing changes.
    p.channel->poll();
    p.channel->poll();
    EXPECT_EQ(p.closes.size(), std::size_t(1));
    EXPECT_EQ(p.closes[0], 1011);
    EXPECT_EQ(p.read(), frame("last words"));
}

TEST_CASE("a full socket queues output; the queue has a limit") {
    Pair p;
    p.channel->poll();
    const std::string big(16384, 'q');
    int sent = 0;
    while (p.channel->send(big) && sent < 200) {
        ++sent;
    }
    EXPECT_LT(sent, 200); // The limit was reached: send() said no.
    EXPECT(p.channel->hasPendingOutput());
    EXPECT(p.channel->isOpen());
    // The reader catches up: everything arrives, in order, nothing lost.
    std::size_t total = 0;
    for (int i = 0; i < 2000 && total < std::size_t(sent) * (4 + big.size()); ++i) {
        p.channel->poll();
        total += p.read().size();
    }
    EXPECT_EQ(total, std::size_t(sent) * (4 + big.size()));
    EXPECT(!p.channel->hasPendingOutput());
}

TEST_CASE("callbacks may send and close") {
    Pair p;
    MessageChannel::Events events;
    UnixSocketChannel *channel = p.channel.get();
    int seen = 0;
    events.message = [&](std::string_view m) {
        ++seen;
        channel->send(std::string("echo:") + std::string(m));
        if (seen == 2) {
            channel->close(1000);
        }
    };
    events.closed = [&](int code) { p.closes.push_back(code); };
    channel->setEvents(std::move(events));
    p.write(frame("a") + frame("b") + frame("c"));
    channel->poll();
    EXPECT_EQ(seen, 2); // No messages after close().
    EXPECT_EQ(p.closes.size(), std::size_t(1));
    EXPECT_EQ(p.read(), frame("echo:a") + frame("echo:b"));
}

TEST_CASE("random bytes never break the channel: messages, or a close, nothing else") {
    std::mt19937 rng(7);
    for (int round = 0; round < 300; ++round) {
        Pair p;
        p.channel->poll();
        std::string noise;
        const int length = int(rng() % 200);
        for (int i = 0; i < length; ++i) {
            // Biased towards small values so some frames are valid.
            noise.push_back(char(rng() % 3 == 0 ? rng() % 256 : rng() % 6));
        }
        p.write(noise);
        if (rng() % 2) {
            p.closePeer();
        }
        for (int i = 0; i < 3; ++i) {
            p.channel->poll();
        }
        EXPECT(p.closes.size() <= 1);
        for (const auto &m : p.messages) {
            EXPECT(!m.empty() && m.size() <= UnixSocketChannel::kMaxMessage);
        }
    }
}

TEST_CASE("connecting to a socket path") {
    char dir[] = "/tmp/mttXXXXXX";
    EXPECT(::mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/s.sock";
    // Nothing there: a clear error that says "no daemon".
    try {
        UnixSocketChannel c(path);
        EXPECT(false);
    }
    catch (const UnixConnectError &e) {
        EXPECT_EQ(e.error(), ENOENT);
    }
    // Too long a path.
    try {
        UnixSocketChannel c(std::string(300, 'a'));
        EXPECT(false);
    }
    catch (const UnixConnectError &e) {
        EXPECT_EQ(e.error(), ENAMETOOLONG);
    }
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    EXPECT_EQ(::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address), 0);
    EXPECT_EQ(::listen(server, 4), 0);
    UnixSocketChannel client(path);
    std::vector<std::string> got;
    MessageChannel::Events events;
    events.message = [&](std::string_view m) { got.emplace_back(m); };
    client.setEvents(std::move(events));
    const int accepted = ::accept(server, nullptr, nullptr);
    EXPECT(accepted >= 0);
    EXPECT(client.send("ping"));
    char buffer[16];
    const auto n = ::read(accepted, buffer, sizeof buffer);
    EXPECT_EQ(std::string(buffer, std::size_t(n)), frame("ping"));
    const auto reply = frame("pong");
    EXPECT_EQ(::write(accepted, reply.data(), reply.size()), ssize_t(reply.size()));
    client.poll();
    EXPECT_EQ(got.size(), std::size_t(1));
    ::close(accepted);
    ::close(server);
    ::unlink(path.c_str());
    ::rmdir(dir);
}

TEST_CASE("a ClientSession runs over it: Hello goes out framed, Welcome is read") {
    Pair p;
    ClientSession::Config config;
    config.clock = [] { return protocol::Time(); };
    config.columns = 90;
    config.rows = 30;
    ClientSession session(*p.channel, config);
    p.channel->poll(); // opened -> Hello
    const auto raw = p.read();
    EXPECT_GT(raw.size(), std::size_t(4));
    const std::size_t length = (std::size_t(std::uint8_t(raw[2])) << 8) | std::uint8_t(raw[3]);
    EXPECT_EQ(raw.size(), 4 + length);
    const auto hello = protocol::decodeClient(raw.substr(4));
    EXPECT(hello);
    EXPECT(std::holds_alternative<protocol::Hello>(*hello.message));
    EXPECT(session.state() == ClientSession::State::AwaitWelcome);
    protocol::Welcome welcome;
    welcome.version = 1;
    std::string bytes;
    EXPECT(protocol::encode(protocol::ServerMessage(welcome), bytes));
    p.write(frame(bytes));
    p.channel->poll();
    EXPECT(session.state() == ClientSession::State::Running);
}

TEST_SUIT_END
