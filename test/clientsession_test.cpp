#include "clientsession.h"
#include "message.h"
#include "mls-unit-test/unittest.h"
#include "serversession.h"

#include <vector>

using namespace protocol;
using std::chrono::milliseconds;

namespace {
// Records what the client sends and lets the test play the server's part.
class FakeChannel : public MessageChannel {
public:
    void setEvents(Events events) override {
        events_ = std::move(events);
    }
    bool send(std::string_view bytes) override {
        if (!open || fullBuffer) {
            return false;
        }
        sent.emplace_back(bytes);
        return true;
    }
    void close(int code) override {
        closeCodes.push_back(code);
        open = false;
    }

    void opened() {
        open = true;
        events_.opened();
    }
    void deliver(const ServerMessage &message) {
        std::string bytes;
        encode(message, bytes);
        events_.message(bytes);
    }
    void deliverRaw(std::string_view bytes) {
        events_.message(bytes);
    }
    void lost(int code) {
        open = false;
        events_.closed(code);
    }
    std::vector<ClientMessage> messages() const {
        std::vector<ClientMessage> result;
        for (const auto &bytes : sent) {
            const auto decoded = decodeClient(bytes);
            result.push_back(decoded ? *decoded.message
                                     : ClientMessage(Close{}));
        }
        return result;
    }

    std::vector<std::string> sent;
    std::vector<int> closeCodes;
    bool open = false;
    bool fullBuffer = false;

private:
    Events events_;
};

struct Fixture {
    FakeChannel channel;
    milliseconds now{1000};
    std::string output;
    std::vector<std::pair<int, int>> exits;
    std::vector<std::string> closes;
    ClientSession session;

    explicit Fixture(ClientSession::Config config = {})
        : session(channel, withClock(config)) {
        session.setEvents(
            {[this](std::string_view bytes) { output += bytes; },
             [this](int code, int signal) { exits.emplace_back(code, signal); },
             [this](std::string_view reason) { closes.emplace_back(reason); }});
    }
    ClientSession::Config withClock(ClientSession::Config config) {
        config.clock = [this] { return now; };
        return config;
    }
    // Opens the channel and completes the handshake.
    void connect() {
        channel.opened();
        channel.deliver(Welcome{1, "abc", 0, 0, 0});
    }
    void advance(int ms) {
        now += milliseconds(ms);
        session.tick();
    }
    std::size_t sentCount() const {
        return channel.sent.size();
    }
};
} // namespace

namespace {
// ServerSession driven by the test, wired to a client over in-memory plumbing.
struct Loopback : SessionHost {
    FakeChannel channel;
    milliseconds now{0};
    ServerSession server;
    std::string clientOutput;
    std::vector<std::pair<int, int>> exits;
    std::vector<std::string> closes;
    ClientSession client;
    std::string processInput;
    bool terminated = false;
    int resizes = 0;

    explicit Loopback(SessionConfig serverConfig = {})
        : server(serverConfig, *this)
        , client(channel, clientConfig()) {
        client.setEvents(
            {[this](std::string_view bytes) { clientOutput += bytes; },
             [this](int code, int signal) { exits.emplace_back(code, signal); },
             [this](std::string_view reason) { closes.emplace_back(reason); }});
        server.start(now);
        channel.opened();
        pump();
    }
    ClientSession::Config clientConfig() {
        ClientSession::Config config;
        config.clock = [this] { return now; };
        return config;
    }
    // Moves everything the client sent into the server.
    void pump() {
        auto pending = std::move(channel.sent);
        channel.sent.clear();
        for (const auto &bytes : pending) {
            server.onMessage(bytes, now);
        }
    }
    void send(std::string bytes) override {
        channel.deliverRaw(bytes);
    }
    void closeTransport(int code) override {
        channel.lost(code);
    }
    bool startProcess(std::uint16_t, std::uint16_t) override {
        return true;
    }
    bool writeInput(std::string_view bytes) override {
        processInput += bytes;
        return true;
    }
    void resizeProcess(std::uint16_t,
                       std::uint16_t,
                       std::uint16_t,
                       std::uint16_t) override {
        ++resizes;
    }
    void terminateProcess() override {
        terminated = true;
    }
};
} // namespace

TEST_SUIT_BEGIN(ClientSession)

TEST_CASE("Replay resets the fake frontend before output and acknowledges "
          "absolute offsets") {
    std::string order;
    ClientSession::Config config;
    config.replay = true;
    config.sessionId = "00112233445566778899aabbccddeeff";
    config.resumeOffset = 12;
    config.resetForReplay = [&] { order += "reset;"; };
    Fixture f(config);
    f.session.setEvents(
        {[&](std::string_view bytes) { order += bytes; }, {}, {}});
    f.channel.opened();
    const auto hello = std::get<Hello>(f.channel.messages().front());
    EXPECT_EQ(hello.sessionId, config.sessionId);
    EXPECT_EQ(hello.resumeOffset, std::uint64_t(12));
    EXPECT_EQ(hello.features, featureReplay);
    f.channel.deliver(Welcome{1, config.sessionId, 20, 3, featureReplay});
    f.channel.deliver(Output{20, "output"});
    EXPECT_EQ(order, std::string("reset;output"));
    EXPECT_EQ(f.session.receivedOffset(), std::uint64_t(26));
    f.advance(100);
    EXPECT_EQ(std::get<Ack>(f.channel.messages().back()).consumedOffset,
              std::uint64_t(26));
}
TEST_CASE("Exact resume keeps frontend state; malformed or unnegotiated replay "
          "fails closed") {
    for (unsigned mode = 0; mode < 5; ++mode) {
        unsigned resets = 0;
        ClientSession::Config config;
        config.replay = mode != 1;
        config.sessionId = "00112233445566778899aabbccddeeff";
        config.resumeOffset = 40;
        if (mode != 2)
            config.resetForReplay = [&] { ++resets; };
        Fixture f(config);
        f.channel.opened();
        auto welcome = Welcome{1, config.sessionId, 40, 1, featureReplay};
        if (mode == 2)
            welcome.flags = 3;
        if (mode == 3)
            welcome.flags = 0x80;
        if (mode == 4)
            welcome.sessionId = "other";
        f.channel.deliver(welcome);
        if (mode == 0) {
            EXPECT(f.session.state() == ClientSession::State::Running);
            EXPECT_EQ(resets, 0u);
        }
        else
            EXPECT(f.session.state() == ClientSession::State::Failed);
    }
}
TEST_CASE(
    "A frontend disconnect during replay reset cannot revive the client") {
    ClientSession::Config config;
    config.replay = true;
    config.sessionId = "00112233445566778899aabbccddeeff";
    Fixture *fixture = nullptr;
    config.resetForReplay = [&] { fixture->channel.lost(1000); };
    Fixture f(config);
    fixture = &f;
    f.channel.opened();
    f.channel.deliver(Welcome{1, config.sessionId, 0, 3, featureReplay});
    EXPECT(f.session.state() == ClientSession::State::Failed);
    f.channel.deliver(Output{0, "late"});
    EXPECT(f.output.empty());
}

TEST_CASE("A zero resume offset requires reconstruction rather than retaining "
          "stale frontend state") {
    ClientSession::Config config;
    config.replay = true;
    config.sessionId = "00112233445566778899aabbccddeeff";
    config.resumeOffset = 0;
    config.resetForReplay = [] {};
    Fixture f(config);
    f.channel.opened();
    f.channel.deliver(
        Welcome{1, config.sessionId, 0, welcomeResumed, featureReplay});
    EXPECT(f.session.state() == ClientSession::State::Failed);
}

TEST_CASE(
    "Replay-capable new terminals require opaque 128-bit lowercase hex ids") {
    for (const auto &id : {std::string(),
                           std::string("abc"),
                           std::string(32, 'g'),
                           std::string("00112233445566778899aabbccddeeff")}) {
        ClientSession::Config config;
        config.replay = true;
        Fixture f(config);
        f.channel.opened();
        f.channel.deliver(Welcome{1, id, 0, 0, featureReplay});
        EXPECT(f.session.state() == (id.size() == 32 && id.front() == '0'
                                         ? ClientSession::State::Running
                                         : ClientSession::State::Failed));
    }
}

TEST_CASE("A hostile replay offset cannot wrap the output counter") {
    ClientSession::Config config;
    config.replay = true;
    config.sessionId = "00112233445566778899aabbccddeeff";
    config.resumeOffset = ~std::uint64_t(0);
    Fixture f(config);
    f.channel.opened();
    f.channel.deliver(
        Welcome{1, config.sessionId, config.resumeOffset, 1, featureReplay});
    f.channel.deliver(Output{config.resumeOffset, "x"});
    EXPECT(f.session.state() == ClientSession::State::Failed);
    EXPECT_EQ(f.session.receivedOffset(), config.resumeOffset);
}

TEST_CASE("Opening the channel sends Hello with the configured size") {
    ClientSession::Config config;
    config.columns = 100;
    config.rows = 30;
    Fixture f(config);
    EXPECT(f.session.state() == ClientSession::State::Connecting);
    EXPECT(f.channel.sent.empty());
    f.channel.opened();
    EXPECT(f.session.state() == ClientSession::State::AwaitWelcome);
    const auto messages = f.channel.messages();
    EXPECT_EQ(messages.size(), std::size_t(1));
    const auto &hello = std::get<Hello>(messages[0]);
    EXPECT_EQ(hello.columns, 100);
    EXPECT_EQ(hello.rows, 30);
    EXPECT(hello.sessionId.empty());
    f.channel.deliver(Welcome{1, "abc", 0, 0, 0});
    EXPECT(f.session.state() == ClientSession::State::Running);
    EXPECT(f.session.sessionId() == "abc");
}

TEST_CASE("A resize before the channel opens changes the size in Hello") {
    Fixture f;
    f.session.resize(120, 40);
    EXPECT(f.channel.sent.empty());
    f.channel.opened();
    EXPECT_EQ(std::get<Hello>(f.channel.messages()[0]).columns, 120);
    EXPECT_EQ(std::get<Hello>(f.channel.messages()[0]).rows, 40);
}

TEST_CASE("Input is refused before opening and chunked afterwards") {
    Fixture f;
    EXPECT_FALSE(f.session.sendInput("x"));
    f.connect();
    const std::string paste(10000, 'p');
    EXPECT(f.session.sendInput(paste));
    std::string joined;
    int chunks = 0;
    for (const auto &message : f.channel.messages()) {
        if (const auto *input = std::get_if<Input>(&message)) {
            EXPECT(input->data.size() <= 4096);
            joined += input->data;
            ++chunks;
        }
    }
    EXPECT_EQ(chunks, 3);
    EXPECT(joined == paste);
    f.channel.fullBuffer = true;
    EXPECT_FALSE(f.session.sendInput("x"));
}

TEST_CASE("Resize is sent as a message, with out of range sizes clamped") {
    Fixture f;
    f.connect();
    f.session.resize(100, 50, 800, 600);
    f.session.resize(5000, 0, -4, 70000);
    const auto messages = f.channel.messages();
    const auto &first = std::get<Resize>(messages[messages.size() - 2]);
    EXPECT_EQ(first.columns, 100);
    EXPECT_EQ(first.pixelWidth, 800);
    const auto &second = std::get<Resize>(messages.back());
    EXPECT_EQ(second.columns, 1000);
    EXPECT_EQ(second.rows, 1);
    EXPECT_EQ(second.pixelWidth, 0);
    EXPECT_EQ(second.pixelHeight, 65535);
}

TEST_CASE("Output is delivered in order and acknowledged by volume or time") {
    ClientSession::Config config;
    config.ackThreshold = 100;
    Fixture f(config);
    f.connect();
    const auto before = f.sentCount();
    f.channel.deliver(Output{0, "hello "});
    f.channel.deliver(Output{6, "world"});
    EXPECT(f.output == "hello world");
    EXPECT_EQ(f.sentCount(), before); // Below the threshold: no Ack yet.
    EXPECT(f.session.nextDeadline() == f.now + milliseconds(100));
    f.advance(99);
    EXPECT_EQ(f.sentCount(), before);
    f.advance(1);
    EXPECT_EQ(f.sentCount(), before + 1);
    EXPECT(std::get<Ack>(f.channel.messages().back()).consumedOffset == 11);
    f.advance(500); // Nothing new to acknowledge.
    EXPECT_EQ(f.sentCount(), before + 1);

    f.channel.deliver(Output{11, std::string(100, 'x')});
    EXPECT_EQ(f.sentCount(), before + 2); // Threshold reached: immediate Ack.
    EXPECT(std::get<Ack>(f.channel.messages().back()).consumedOffset == 111);
}

TEST_CASE("A gap or overlap in the output stream is a protocol error") {
    {
        Fixture f;
        f.connect();
        f.channel.deliver(Output{0, "abc"});
        f.channel.deliver(Output{4, "d"});
        EXPECT(f.session.state() == ClientSession::State::Failed);
        EXPECT_EQ(f.closes.size(), std::size_t(1));
        EXPECT(f.channel.closeCodes == std::vector<int>{1002});
        EXPECT(f.output == "abc");
    }
    {
        Fixture f;
        f.connect();
        f.channel.deliver(Output{0, "abc"});
        f.channel.deliver(Output{2, "cd"});
        EXPECT(f.session.state() == ClientSession::State::Failed);
    }
}

TEST_CASE("Exit is reported once, after a final Ack, and is not an error") {
    Fixture f;
    f.connect();
    f.channel.deliver(Output{0, "bye"});
    f.channel.deliver(Exit{7, 0});
    EXPECT(f.session.state() == ClientSession::State::Exited);
    EXPECT_EQ(f.exits.size(), std::size_t(1));
    EXPECT_EQ(f.exits[0].first, 7);
    EXPECT(std::get<Ack>(f.channel.messages().back()).consumedOffset == 3);
    f.channel.lost(1000); // The server closes after Exit.
    EXPECT(f.closes.empty());
    EXPECT_FALSE(f.session.sendInput("x"));
    EXPECT_FALSE(f.session.nextDeadline().has_value());
}

TEST_CASE("Server errors and lost connections are reported once") {
    {
        Fixture f;
        f.connect();
        f.channel.deliver(protocol::Error{ErrorCode::Timeout, "idle"});
        EXPECT(f.session.state() == ClientSession::State::Failed);
        EXPECT_EQ(f.closes.size(), std::size_t(1));
        EXPECT(f.closes[0].find("Timeout") != std::string::npos);
        EXPECT(f.closes[0].find("idle") != std::string::npos);
        EXPECT(f.session.errorCode() == ErrorCode::Timeout);
        f.channel.lost(1008);
        EXPECT(f.session.errorCode() == ErrorCode::Timeout);
        EXPECT_EQ(f.closes.size(), std::size_t(1));
    }
    {
        Fixture f;
        f.connect();
        f.channel.lost(1006);
        EXPECT(f.session.state() == ClientSession::State::Failed);
        EXPECT_EQ(f.closes.size(), std::size_t(1));
        EXPECT(f.closes[0].find("1006") != std::string::npos);
        EXPECT_FALSE(f.session.errorCode().has_value());
    }
}

TEST_CASE("Bad or out of order messages fail the session") {
    {
        Fixture f;
        f.channel.opened();
        f.channel.deliver(Output{0, "x"}); // Before Welcome.
        EXPECT(f.session.state() == ClientSession::State::Failed);
    }
    {
        Fixture f;
        f.connect();
        f.channel.deliver(Welcome{1, "again", 0, 0, 0});
        EXPECT(f.session.state() == ClientSession::State::Failed);
    }
    {
        Fixture f;
        f.connect();
        f.channel.deliverRaw(std::string("\x02x", 2)); // A client type.
        EXPECT(f.session.state() == ClientSession::State::Failed);
        EXPECT(f.channel.closeCodes == std::vector<int>{1002});
    }
    {
        Fixture f;
        f.channel.opened();
        f.channel.deliver(Welcome{2, "future", 0, 0, 0}); // Unknown version.
        EXPECT(f.session.state() == ClientSession::State::Failed);
    }
}

TEST_CASE("Idle connections send Ping and a missing Pong is fatal") {
    Fixture f;
    f.connect();
    EXPECT(f.session.nextDeadline() == f.now + milliseconds(20000));
    f.advance(19999);
    EXPECT_EQ(f.sentCount(), std::size_t(1)); // Only Hello so far.
    f.advance(1);
    EXPECT_EQ(f.sentCount(), std::size_t(2));
    const auto nonce = std::get<Ping>(f.channel.messages().back()).nonce;
    f.advance(29999);
    EXPECT(f.session.state() == ClientSession::State::Running);
    f.advance(1);
    EXPECT(f.session.state() == ClientSession::State::Failed);
    EXPECT_EQ(f.closes.size(), std::size_t(1));
    (void)nonce;
}

TEST_CASE("A Pong or any server traffic keeps the connection alive") {
    Fixture f;
    f.connect();
    f.advance(20000);
    const auto nonce = std::get<Ping>(f.channel.messages().back()).nonce;
    f.channel.deliver(Pong{nonce});
    f.advance(10000); // No new ping yet: idle time restarted at the Ping.
    EXPECT(f.session.state() == ClientSession::State::Running);
    f.advance(10000);
    EXPECT(std::holds_alternative<Ping>(f.channel.messages().back()));
    // Output instead of a Pong also proves the server is alive.
    f.advance(25000);
    f.channel.deliver(Output{0, "x"});
    f.advance(29000);
    EXPECT(f.session.state() == ClientSession::State::Running);
}

TEST_CASE("Close sends Close and keeps listening for Exit") {
    Fixture f;
    f.connect();
    f.session.close();
    EXPECT(std::holds_alternative<Close>(f.channel.messages().back()));
    f.channel.deliver(Exit{-1, 1});
    EXPECT_EQ(f.exits.size(), std::size_t(1));
    EXPECT_EQ(f.exits[0].second, 1);
}

TEST_CASE("The transport close code is reported, and only when it closed") {
    {
        // A normal close after our Close is distinguishable from a network
        // failure without reading the human text.
        Fixture f;
        f.connect();
        EXPECT(!f.session.closeCode().has_value());
        f.session.close();
        f.channel.lost(1000);
        EXPECT(f.session.closeCode() == std::optional<int>(1000));
        EXPECT(!f.session.errorCode().has_value());
    }
    {
        Fixture f;
        f.connect();
        f.channel.lost(1006);
        EXPECT(f.session.closeCode() == std::optional<int>(1006));
    }
    {
        // A protocol error closes the channel itself: no code from the peer.
        Fixture f;
        f.connect();
        f.channel.deliver(Error{ErrorCode::Replaced, "session taken over"});
        EXPECT(!f.session.closeCode().has_value());
        EXPECT(f.session.errorCode() ==
               std::optional<ErrorCode>(ErrorCode::Replaced));
    }
    {
        // A normal exit is not a close.
        Fixture f;
        f.connect();
        f.channel.deliver(Exit{0, 0});
        EXPECT(!f.session.closeCode().has_value());
    }
}

TEST_CASE("Client and server sessions work together end to end") {
    Loopback l;
    EXPECT(l.client.state() == ClientSession::State::Running);
    EXPECT(l.client.sessionId() == "session");

    l.client.sendInput("ls\r");
    l.pump();
    EXPECT(l.processInput == "ls\r");
    l.server.onProcessOutput("a.txt\r\n", l.now);
    EXPECT(l.clientOutput == "a.txt\r\n");

    l.client.resize(100, 40, 900, 700);
    l.pump();
    l.now += milliseconds(100); // Resizes are coalesced for 100 ms.
    l.server.tick(l.now);
    EXPECT_EQ(l.resizes, 1);

    l.client.close();
    l.pump();
    EXPECT(l.terminated);
    l.server.onProcessExit(-1, 1, l.now);
    EXPECT_EQ(l.exits.size(), std::size_t(1));
    EXPECT_EQ(l.exits[0].second, 1);
    EXPECT(l.closes.empty());
}

TEST_CASE("Flow control keeps a large output flowing as the client acks") {
    SessionConfig serverConfig;
    serverConfig.outputWindow = 4096;
    Loopback l(serverConfig);
    const std::string chunk(1000, 'z');
    std::size_t total = 0;
    for (int round = 0; round < 200; ++round) {
        const auto budget = l.server.outputBudget();
        if (budget == 0) {
            // The window is full: the client's tick-driven Ack frees it.
            l.now += milliseconds(100);
            l.client.tick();
            l.pump();
            EXPECT(l.server.outputBudget() > 0);
            continue;
        }
        const auto size = std::min(budget, chunk.size());
        l.server.onProcessOutput(std::string_view(chunk).substr(0, size),
                                 l.now);
        total += size;
        l.pump();
    }
    EXPECT(total > 100000);
    EXPECT_EQ(l.clientOutput.size(), total);
    EXPECT(l.client.state() == ClientSession::State::Running);
    EXPECT(l.server.state() == ServerSession::State::Running);
}

TEST_CASE("Long idle periods stay alive through client pings") {
    Loopback l;
    for (int i = 0; i < 10; ++i) {
        l.now += milliseconds(20000);
        l.client.tick();
        l.pump();
        l.server.tick(l.now);
    }
    EXPECT(l.server.state() == ServerSession::State::Running);
    EXPECT(l.client.state() == ClientSession::State::Running);
}

TEST_SUIT_END
