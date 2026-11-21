// Threat-model checks of the chat server over real loopback sockets: a hostile
// client trying the proxy header, the Origin check, the test listener, slow
// requests, malformed HTTP and WebSocket traffic. Everything here passes; gaps
// are in chatissues_test.cpp (docs/CHAT_REVIEW.md).
#include "chatdfixture.h"
#include "mls-unit-test/unittest.h"

#include <chrono>

namespace {
using Clock = std::chrono::steady_clock;

tcp::socket rawConnect(net::io_context &io, std::uint16_t port) {
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    return socket;
}
void sendRaw(tcp::socket &socket, const std::string &bytes) {
    boost::system::error_code ec;
    net::write(socket, net::buffer(bytes), ec);
}
} // namespace

TEST_SUIT_BEGIN(ChatdSecurity)

TEST_CASE("X-Forwarded-For from an untrusted peer never makes a new source") {
    chatd::Options o = quick();
    o.room.maxPerSource = 3; // All of these are really 127.0.0.1.
    Running r(o);
    int joined = 0;
    std::vector<std::unique_ptr<WsClient>> clients;
    for (int i = 0; i < 6; ++i) {
        auto client = std::make_unique<WsClient>(
            r.server->wsPort(),
            kOrigin,
            WsClient::Headers{
                {"X-Forwarded-For", "10.9.8." + std::to_string(i + 1)}});
        if (!client->handshake()) {
            continue; // Refusing the header is also safe.
        }
        client->send("hello\tn" + std::to_string(i));
        if (client->expect("welcome")) {
            ++joined;
        }
        clients.push_back(std::move(client));
    }
    EXPECT(joined <= 3);
    // A forged header that is not even an address is no way in either.
    WsClient junk(r.server->wsPort(),
                  kOrigin,
                  {{"X-Forwarded-For", "\x01\x02 not an ip, 1.2.3.4"}});
    if (junk.handshake()) {
        junk.send("hello\tjunk");
        EXPECT(!junk.expect("welcome") || joined < 3);
    }
}

TEST_CASE("Only the exact Origin passes, in every disguise") {
    Running r(quick());
    for (const char *origin : {"https://chat.example.org/",
                               "HTTPS://chat.example.org",
                               "https://CHAT.example.org",
                               "https://chat.example.org:443",
                               "https://chat.example.org:8443",
                               "http://chat.example.org",
                               "https://chat.example.org.evil.example",
                               "https://evilchat.example.org",
                               "https://chat.example.org@evil.example",
                               "https://evil.example/https://chat.example.org",
                               "null",
                               "*",
                               "chat.example.org",
                               "file://",
                               "https://chat.example.org\tevil"}) {
        WsClient client(r.server->wsPort(), origin);
        EXPECT(!client.handshake());
    }
    // (Spaces around the value are no disguise: HTTP drops them, the value is
    // then exact.) Two Origin headers, one of them ours, are refused rather
    // than guessed at.
    WsClient twice(
        r.server->wsPort(), kOrigin, {{"Origin", "https://evil.example"}});
    EXPECT(!twice.handshake());
    WsClient right(r.server->wsPort(), kOrigin);
    EXPECT(right.handshake());
}

TEST_CASE("The plain TCP test listener is loopback only, in every spelling") {
    for (const char *host : {"0.0.0.0",
                             "::",
                             "10.1.2.3",
                             "192.168.0.1",
                             "8.8.8.8",
                             "::ffff:10.0.0.1",
                             "::ffff:0.0.0.0",
                             "localhost",
                             "127.0.0.1 ",
                             " 127.0.0.1",
                             "127.0.0.1.evil",
                             "0",
                             "",
                             "[::1]",
                             "128.0.0.1",
                             "126.255.255.255"}) {
        net::io_context io;
        chatd::Options o;
        o.origins = {kOrigin};
        o.tcpEnabled = true;
        o.tcpHost = host;
        bool refused = false;
        try {
            chatd::ChatServer server(io, o);
        }
        catch (const std::invalid_argument &) {
            refused = true;
        }
        EXPECT(refused);
    }
    for (const char *host : {"127.0.0.1", "127.5.5.5", "::ffff:127.0.0.1"}) {
        net::io_context io;
        chatd::Options o;
        o.origins = {kOrigin};
        o.tcpEnabled = true;
        o.tcpHost = host;
        bool accepted = true;
        try {
            chatd::ChatServer server(io, o);
        }
        catch (const std::exception &) {
            accepted = false;
        }
        EXPECT(accepted);
    }
}

TEST_CASE("Without the test flag nothing listens for plain TCP") {
    net::io_context io;
    chatd::Options o;
    o.origins = {kOrigin};
    chatd::ChatServer server(io, o);
    EXPECT_EQ(server.tcpPort(), 0);
}

TEST_CASE("A client that never finishes its request is dropped, however slowly "
          "it types") {
    Running r(quick());
    net::io_context io;
    auto silent = rawConnect(io, r.server->wsPort());
    sendRaw(silent, "GET / HTTP/1.1\r\nHost: x\r\n");
    const auto start = Clock::now();
    EXPECT(closedWithin(silent, 9));
    EXPECT(Clock::now() - start < std::chrono::seconds(8));
    // One byte every half second does not extend the deadline.
    auto trickle = rawConnect(io, r.server->wsPort());
    sendRaw(trickle, "GET / HTTP/1.1\r\n");
    setReadTimeout(trickle.native_handle(), 1);
    const auto begin = Clock::now();
    boost::system::error_code ec;
    while (!ec && Clock::now() - begin < std::chrono::seconds(9)) {
        net::write(trickle, net::buffer(std::string("X: a\r\n")), ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT(ec);
    EXPECT(Clock::now() - begin < std::chrono::seconds(8));
}

TEST_CASE("Malformed or oversized HTTP is refused, not parsed") {
    Running r(quick());
    net::io_context io;
    const std::string upgrade =
        "Host: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\nOrigin: https://chat.example.org\r\n";
    // A header that is too big.
    {
        auto socket = rawConnect(io, r.server->wsPort());
        sendRaw(socket,
                "GET / HTTP/1.1\r\n" + upgrade +
                    "X-Big: " + std::string(20000, 'a') + "\r\n\r\n");
        EXPECT(closedWithin(socket, 3));
    }
    // A body on the upgrade request.
    {
        auto socket = rawConnect(io, r.server->wsPort());
        sendRaw(socket,
                "GET / HTTP/1.1\r\n" + upgrade +
                    "Content-Length: 5\r\n\r\nhello");
        EXPECT(closedWithin(socket, 3));
    }
    // Another method, another path, garbage.
    for (const char *request : {"POST / HTTP/1.1\r\n",
                                "GET /chat HTTP/1.1\r\n",
                                "GET / HTTP/1.0\r\n",
                                "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03",
                                "GET / HTTP/1.1\r\nHost: x\r\n\r\n"}) {
        auto socket = rawConnect(io, r.server->wsPort());
        std::string text = request;
        if (text.back() == '\n' && text.find("\r\n\r\n") == std::string::npos) {
            text += upgrade + "\r\n";
        }
        sendRaw(socket, text);
        EXPECT(closedWithin(socket, 3));
    }
}

TEST_CASE(
    "Fragmented messages are bounded as a whole, and a ping flood is cheap") {
    Running r(quick());
    WsClient a(r.server->wsPort(), kOrigin);
    EXPECT(a.handshake());
    // Two fragments that together are over the frame limit.
    a.stream.text(true);
    beast::error_code ec;
    a.stream.write_some(false, net::buffer(std::string(600, 'x')), ec);
    a.stream.write_some(true, net::buffer(std::string(600, 'y')), ec);
    std::string reply = a.read();
    while (reply != "<closed>" && reply.compare(0, 5, "error") == 0) {
        reply = a.read();
    }
    EXPECT(reply == "<closed>");
    // Thousands of pings from one client leave the room usable for others.
    WsClient flooder(r.server->wsPort(), kOrigin);
    EXPECT(flooder.handshake());
    for (int i = 0; i < 3000; ++i) {
        flooder.stream.ping({}, ec);
        if (ec) {
            break;
        }
    }
    WsClient b(r.server->wsPort(), kOrigin);
    EXPECT(b.handshake());
    b.send("hello\tbo");
    EXPECT(b.expect("ready"));
}

TEST_CASE(
    "The console never shows anything a client chose except a valid name") {
    Running r(quick());
    Line a(r.server->tcpPort());
    a.send("hello\tada");
    EXPECT(a.expect("ready"));
    Line eve(r.server->tcpPort());
    eve.send("hello\t\x1b[2Jeve");
    EXPECT(eve.expect("error"));
    const auto listing = r.console("list");
    for (const char c : listing) {
        EXPECT(c == '\n' || (c >= 0x20 && c < 0x7f));
    }
}

TEST_SUIT_END
