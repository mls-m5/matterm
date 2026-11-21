#include "chatserver.h"
#include "mls-unit-test/unittest.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <thread>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;

const char *kOrigin = "https://chat.example.org";

// A server on free loopback ports, running on its own thread.
struct Running {
    explicit Running(chatd::Options options) {
        options.origins = {kOrigin};
        options.tcpEnabled = true;
        server = std::make_unique<chatd::ChatServer>(io, options);
        server->start();
        thread = std::thread([this] { io.run(); });
    }
    ~Running() {
        net::post(io, [this] { server->stop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        io.stop();
        thread.join();
    }
    std::string console(const std::string &command) {
        std::promise<std::string> promise;
        auto future = promise.get_future();
        net::post(io, [&] { promise.set_value(server->console(command)); });
        return future.get();
    }
    net::io_context io;
    std::unique_ptr<chatd::ChatServer> server;
    std::thread thread;
};

// A blocking client for the plain TCP test listener, with a read timeout.
class Line {
public:
    explicit Line(std::uint16_t port)
        : socket_(io_) {
        socket_.connect(
            tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        timeval tv{2, 0};
        setsockopt(
            socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    void send(const std::string &line) {
        const std::string data = line + "\n";
        net::write(socket_, net::buffer(data));
    }
    // Reads lines until one starts with `prefix`; false on timeout or end.
    bool expect(const std::string &prefix) {
        while (true) {
            std::string l;
            if (!readLine(l)) {
                return false;
            }
            seen.push_back(l);
            if (l.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
    }
    // True when the other side closes within the timeout.
    bool ends() {
        std::string l;
        while (readLine(l)) {
            seen.push_back(l);
        }
        return eof_;
    }
    bool saw(const std::string &prefix) const {
        for (const auto &l : seen) {
            if (l.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return false;
    }
    std::vector<std::string> seen;

private:
    bool readLine(std::string &out) {
        while (true) {
            const auto nl = buffer_.find('\n');
            if (nl != std::string::npos) {
                out = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                return true;
            }
            char chunk[512];
            boost::system::error_code ec;
            const auto n = socket_.read_some(net::buffer(chunk), ec);
            if (ec) {
                eof_ =
                    ec == net::error::eof || ec == net::error::connection_reset;
                return false;
            }
            buffer_.append(chunk, n);
        }
    }
    net::io_context io_;
    tcp::socket socket_;
    std::string buffer_;
    bool eof_ = false;
};

chatd::Options quick() {
    chatd::Options o;
    o.room.lockBackstop = 400;
    o.room.draftForward = 50;
    o.room.maxPerSource = 8; // Everyone is 127.0.0.1 here.
    return o;
}
} // namespace

TEST_SUIT_BEGIN(ChatdTcp)

TEST_CASE(
    "Two clients over TCP: join, live draft, lock, history for a latecomer") {
    Running r(quick());
    Line ada(r.server->tcpPort());
    ada.send("hello\tada");
    EXPECT(ada.expect("ready"));
    Line bo(r.server->tcpPort());
    bo.send("hello\tbo");
    EXPECT(bo.expect("ready"));
    EXPECT(ada.expect("join\tbo"));
    ada.send("draft\t1\tanyone around?");
    EXPECT(bo.expect("draft\tada\tanyone around?"));
    ada.send("lock\t1");
    EXPECT(bo.expect("msg\t1\tada\tanyone around?"));
    EXPECT(ada.expect("ack\t1\t1\tanyone around?"));
    Line cy(r.server->tcpPort());
    cy.send("hello\tcy");
    EXPECT(cy.expect("hist\t1\t"));
    EXPECT(cy.expect("ready"));
    // The server's own backstop locks a bubble that nobody locked.
    bo.send("draft\t1\tstill typing");
    EXPECT(ada.expect("msg\t2\tbo\tstill typing"));
}

TEST_CASE("Hostile lines are refused and never reach the others") {
    Running r(quick());
    Line ada(r.server->tcpPort());
    ada.send("hello\tada");
    EXPECT(ada.expect("ready"));
    Line eve(r.server->tcpPort());
    eve.send("hello\teve");
    EXPECT(eve.expect("ready"));
    eve.send("draft\t1\t\x1b]52;c;AAAA\x07");
    EXPECT(eve.expect("error\tbad-line"));
    eve.send("draft\t1\tsmile \xF0\x9F\x98\x80");
    EXPECT(eve.expect("error\tbad-line"));
    eve.send("draft\t1\t\xE2\x80\xAE"
             "evil");
    EXPECT(eve.expect("error\tbad-line"));
    eve.send("draft\t1\tlegit");
    EXPECT(ada.expect("draft\teve\tlegit"));
    for (const auto &l : ada.seen) {
        EXPECT(l.find('\x1b') == std::string::npos);
        EXPECT(l.find("evil") == std::string::npos);
    }
}

TEST_CASE("A flood and a line that is too long end the connection") {
    Running r(quick());
    Line flooder(r.server->tcpPort());
    flooder.send("hello\tflo");
    EXPECT(flooder.expect("ready"));
    for (int i = 0; i < 60; ++i) {
        flooder.send("draft\t1\ta" + std::to_string(i));
    }
    EXPECT(flooder.ends());
    Line longline(r.server->tcpPort());
    longline.send(std::string(5000, 'x'));
    EXPECT(longline.ends());
}

TEST_CASE("A name is taken until its owner leaves, then it can be taken over") {
    Running r(quick());
    Line second(r.server->tcpPort());
    {
        Line first(r.server->tcpPort());
        first.send("hello\tada");
        EXPECT(first.expect("ready"));
        second.send("hello\tAda");
        EXPECT(second.expect("error\tname-taken"));
    } // The owner's connection closes here.
    for (int i = 0; i < 40 && !second.saw("welcome\tAda"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        second.send("hello\tAda");
        if (second.expect("welcome\tAda")) {
            break;
        }
    }
    EXPECT(second.saw("welcome\tAda"));
}

TEST_CASE("The per-source cap and the room being locked") {
    chatd::Options o = quick();
    o.room.maxPerSource = 2;
    Running r(o);
    Line a(r.server->tcpPort());
    a.send("hello\tada");
    EXPECT(a.expect("ready"));
    Line b(r.server->tcpPort());
    b.send("hello\tbo");
    EXPECT(b.expect("ready"));
    Line c(r.server->tcpPort());
    EXPECT(c.expect("error\tfull"));
    EXPECT(c.ends());
    EXPECT(r.console("stats").find("online 2") != std::string::npos);
    r.console("lock");
    EXPECT(r.console("stats").find("LOCKED") != std::string::npos);
    r.console("say hello all");
    EXPECT(a.expect("notice\thello all"));
    r.console("kick bo");
    EXPECT(b.expect("error\tbanned"));
    EXPECT(b.ends());
}

TEST_CASE("Fuzz: garbage, fragments and floods over TCP leave the server up "
          "and clean") {
    chatd::Options o = quick();
    o.room.maxPerSource = 64;
    o.room.connectBurst = 1000000; // Many connections from one address.
    o.maxPendingPerPeer = 1000;
    o.room.violationsToKick =
        100000; // Everyone is 127.0.0.1: no bans in this test.
    Running r(o);
    std::uint64_t state = 4242;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return std::uint32_t(state >> 33);
    };
    const char *pieces[] = {"hello\tmal",
                            "draft\t1\thi",
                            "lock\t1",
                            "\n",
                            "\r\n",
                            "\t",
                            "\x1b[2J",
                            "\xff\xfe",
                            "nick\tzed",
                            "draft\t999999999\tx",
                            std::string(300, 'a').c_str(),
                            "msg\t1\tbo\tx",
                            "\x00"};
    for (int round = 0; round < 40; ++round) {
        Line chaos(r.server->tcpPort());
        std::string blob;
        const int n = 1 + int(next() % 30);
        for (int i = 0; i < n; ++i) {
            blob += pieces[next() % (sizeof pieces / sizeof *pieces)];
            if (next() % 4 == 0) {
                blob += char(next() % 256);
            }
        }
        // Raw bytes, as they come: no added newline.
        try {
            chaos.send(blob);
        }
        catch (...) {
        }
        // What comes back is only ever well-formed lines of ours.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The server is still there and behaves.
    Line honest(r.server->tcpPort());
    honest.send("hello\thonest");
    EXPECT(honest.expect("ready"));
    honest.send("draft\t1\tstill works");
    honest.send("lock\t1");
    EXPECT(honest.expect("ack\t1\t"));
    for (const auto &l : honest.seen) {
        EXPECT(l.find('\x1b') == std::string::npos);
    }
    // The chaos connections are gone (or at least not counted forever).
    for (int i = 0; i < 100; ++i) {
        if (r.server->stats().connections == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(r.server->stats().connections, std::size_t(1));
}

TEST_CASE("The options that would be unsafe are refused") {
    net::io_context io;
    chatd::Options open;
    open.tcpEnabled = true;
    open.tcpHost = "0.0.0.0"; // Not loopback.
    open.origins = {kOrigin};
    EXPECT_THROW(chatd::ChatServer(io, open), std::invalid_argument);
    chatd::Options noOrigin;
    noOrigin.tcpEnabled = false;
    EXPECT_THROW(chatd::ChatServer(io, noOrigin), std::invalid_argument);
    chatd::Options badOrigin;
    badOrigin.origins = {"chat.example.org"};
    EXPECT_THROW(chatd::ChatServer(io, badOrigin), std::invalid_argument);
    chatd::Options pathOrigin;
    pathOrigin.origins = {"https://chat.example.org/path"};
    EXPECT_THROW(chatd::ChatServer(io, pathOrigin), std::invalid_argument);
}

TEST_SUIT_END

namespace {
// A Beast client, to see what a browser would see.
struct WsClient {
    explicit WsClient(std::uint16_t port, const std::string &origin)
        : stream(io) {
        stream.next_layer().connect(
            tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
        timeval tv{2, 0};
        setsockopt(stream.next_layer().native_handle(),
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &tv,
                   sizeof tv);
        stream.set_option(
            ws::stream_base::decorator([origin](ws::request_type &req) {
                if (!origin.empty()) {
                    req.set(http::field::origin, origin);
                }
            }));
    }
    bool handshake() {
        beast::error_code ec;
        stream.handshake("127.0.0.1", "/", ec);
        return !ec;
    }
    std::string read() {
        beast::flat_buffer buffer;
        beast::error_code ec;
        stream.read(buffer, ec);
        return ec ? std::string("<closed>")
                  : beast::buffers_to_string(buffer.data());
    }
    net::io_context io;
    ws::stream<tcp::socket> stream;
};
} // namespace

TEST_SUIT_BEGIN(ChatdWebSocket)

TEST_CASE("Only the allowed origin gets in") {
    Running r(quick());
    WsClient none(r.server->wsPort(), "");
    EXPECT(!none.handshake());
    WsClient wrong(r.server->wsPort(), "https://evil.example.org");
    EXPECT(!wrong.handshake());
    WsClient right(r.server->wsPort(), kOrigin);
    EXPECT(right.handshake());
}

TEST_CASE("Text frames work, binary frames and big frames end the connection") {
    Running r(quick());
    WsClient a(r.server->wsPort(), kOrigin);
    EXPECT(a.handshake());
    a.stream.text(true);
    a.stream.write(net::buffer(std::string("hello\tada")));
    EXPECT(a.read().compare(0, 11, "welcome\tada") == 0);
    // The room is shared with the TCP listener.
    Line tcpSide(r.server->tcpPort());
    tcpSide.send("hello\tbo");
    EXPECT(tcpSide.expect("ready"));
    EXPECT(tcpSide.saw("users\tada,bo") || tcpSide.expect("users"));
    WsClient b(r.server->wsPort(), kOrigin);
    EXPECT(b.handshake());
    b.stream.binary(true);
    b.stream.write(net::buffer(std::string("hello\tbinary")));
    std::string first = b.read();
    EXPECT(first.compare(0, 5, "error") == 0 || first == "<closed>");
    WsClient c(r.server->wsPort(), kOrigin);
    EXPECT(c.handshake());
    c.stream.text(true);
    beast::error_code ec;
    c.stream.write(net::buffer(std::string(4000, 'x')), ec);
    std::string reply = c.read();
    while (reply != "<closed>" && reply.compare(0, 5, "error") == 0) {
        reply = c.read();
    }
    EXPECT(reply == "<closed>");
}

TEST_SUIT_END
