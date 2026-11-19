#include "mls-unit-test/unittest.h"
#include "serversession.h"

#include <vector>

using namespace protocol;
using std::chrono::milliseconds;

namespace {
struct FakeHost : SessionHost {
    std::vector<std::string> sent;
    std::vector<int> closes;
    std::vector<std::string> input;
    struct Size {
        int columns, rows, pixelWidth, pixelHeight;
    };
    std::vector<Size> resizes;
    int terminations = 0;
    int starts = 0;
    int startColumns = 0, startRows = 0;
    bool startOk = true;
    bool inputOk = true;

    void send(std::string bytes) override {
        sent.push_back(std::move(bytes));
    }
    void closeTransport(int code) override {
        closes.push_back(code);
    }
    bool startProcess(std::uint16_t columns, std::uint16_t rows) override {
        ++starts;
        startColumns = columns;
        startRows = rows;
        return startOk;
    }
    bool writeInput(std::string_view bytes) override {
        input.emplace_back(bytes);
        return inputOk;
    }
    void resizeProcess(std::uint16_t columns,
                       std::uint16_t rows,
                       std::uint16_t pixelWidth,
                       std::uint16_t pixelHeight) override {
        resizes.push_back({columns, rows, pixelWidth, pixelHeight});
    }
    void terminateProcess() override {
        ++terminations;
    }

    // Decoded view of what the client would have received.
    std::vector<ServerMessage> messages() const {
        std::vector<ServerMessage> result;
        for (const auto &bytes : sent) {
            auto decoded = decodeServer(bytes);
            // An undecodable message shows up as a distinctive error.
            result.push_back(decoded ? *decoded.message
                                     : ServerMessage(Error{ErrorCode::Internal,
                                                           "undecodable"}));
        }
        return result;
    }
    bool lastIsError(ErrorCode code) const {
        const auto all = messages();
        if (all.empty() || !std::holds_alternative<Error>(all.back())) {
            return false;
        }
        return std::get<Error>(all.back()).code == code;
    }
};

std::string bytes(const ClientMessage &message) {
    std::string out;
    encode(message, out);
    return out;
}

struct Fixture {
    FakeHost host;
    SessionConfig config;
    ServerSession session;
    milliseconds now{1000};

    explicit Fixture(SessionConfig c = {})
        : config(c)
        , session(config, host) {
        session.start(now);
    }
    void send(const ClientMessage &message) {
        session.onMessage(bytes(message), now);
    }
    void advance(int ms) {
        now += milliseconds(ms);
        session.tick(now);
    }
    // Completes the handshake with a given terminal size.
    void hello(std::uint16_t columns = 80, std::uint16_t rows = 24) {
        Hello message;
        message.columns = columns;
        message.rows = rows;
        send(message);
    }
};
} // namespace

TEST_SUIT_BEGIN(ServerSession)
TEST_CASE(
    "Registry-selected resume starts its ACK window at an absolute offset") {
    SessionConfig config;
    config.sessionId = "00112233445566778899aabbccddeeff";
    config.startOffset = 100;
    config.resume = true;
    config.features = featureReplay;
    config.welcomeFlags = welcomeResumed;
    Fixture f(config);
    f.send(Hello{1, 80, 24, 100, config.sessionId, featureReplay});
    EXPECT_EQ(f.session.sentOffset(), std::uint64_t(100));
    EXPECT_EQ(f.session.ackedOffset(), std::uint64_t(100));
    EXPECT_EQ(std::get<Welcome>(f.host.messages().front()).startOffset,
              std::uint64_t(100));
    f.session.onProcessOutput("tail", f.now);
    EXPECT_EQ(f.session.sentOffset(), std::uint64_t(104));
    f.send(Ack{104});
    EXPECT_EQ(f.session.outputBudget(), config.outputWindow);
}
TEST_CASE("Absolute output counters fail closed rather than wrapping") {
    SessionConfig config;
    config.startOffset = ~std::uint64_t(0);
    Fixture f(config);
    f.hello();
    f.session.onProcessOutput("x", f.now);
    EXPECT(f.host.lastIsError(ErrorCode::LimitExceeded));
    EXPECT_EQ(f.session.sentOffset(), config.startOffset);
}

TEST_CASE("Hello starts the program and is answered with Welcome") {
    Fixture f;
    EXPECT(f.session.state() == ServerSession::State::AwaitHello);
    EXPECT_EQ(f.host.starts, 0);
    f.hello(100, 30);
    EXPECT(f.session.state() == ServerSession::State::Running);
    EXPECT_EQ(f.host.starts, 1);
    EXPECT_EQ(f.host.startColumns, 100);
    EXPECT_EQ(f.host.startRows, 30);
    const auto messages = f.host.messages();
    EXPECT_EQ(messages.size(), std::size_t(1));
    const auto &welcome = std::get<Welcome>(messages[0]);
    EXPECT_EQ(welcome.version, 1);
    EXPECT(welcome.sessionId == "session");
    EXPECT(welcome.startOffset == 0);
}

TEST_CASE("A newer client version is answered with version 1") {
    Fixture f;
    Hello hello;
    hello.version = 9;
    f.send(hello);
    EXPECT(f.session.state() == ServerSession::State::Running);
    EXPECT_EQ(std::get<Welcome>(f.host.messages()[0]).version, 1);
}

TEST_CASE("Version 0 and resume requests are refused") {
    {
        Fixture f;
        Hello hello;
        hello.version = 0;
        f.send(hello);
        EXPECT(f.host.lastIsError(ErrorCode::UnsupportedVersion));
        EXPECT(f.host.closes == std::vector<int>{1002});
        EXPECT_EQ(f.host.starts, 0);
        EXPECT(f.session.state() == ServerSession::State::Closed);
    }
    {
        Fixture f;
        Hello hello;
        hello.sessionId = "old";
        f.send(hello);
        EXPECT(f.host.lastIsError(ErrorCode::ResumeUnsupported));
        EXPECT(f.host.closes == std::vector<int>{1008});
    }
    {
        Fixture f;
        Hello hello;
        hello.resumeOffset = 5;
        f.send(hello);
        EXPECT(f.host.lastIsError(ErrorCode::ResumeUnsupported));
    }
}

TEST_CASE("A program that cannot start fails the session") {
    Fixture f;
    f.host.startOk = false;
    f.hello();
    EXPECT(f.host.lastIsError(ErrorCode::StartFailed));
    EXPECT(f.host.closes == std::vector<int>{1011});
    EXPECT_EQ(f.host.terminations, 0); // Nothing was started.
}

TEST_CASE("Only Hello is accepted first and a second Hello is refused") {
    {
        Fixture f;
        f.send(Input{"x"});
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
        EXPECT(f.host.input.empty());
    }
    {
        Fixture f;
        f.hello();
        f.hello();
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
        EXPECT_EQ(f.host.terminations, 1);
        EXPECT_EQ(f.host.starts, 1);
    }
}

TEST_CASE("Missing Hello times out after the configured time") {
    Fixture f;
    f.advance(4999);
    EXPECT(f.session.state() == ServerSession::State::AwaitHello);
    EXPECT(f.session.nextDeadline() == milliseconds(6000));
    f.advance(1);
    EXPECT(f.host.lastIsError(ErrorCode::Timeout));
    EXPECT(f.host.closes == std::vector<int>{1008});
}

TEST_CASE("Malformed and oversized messages are fatal") {
    {
        Fixture f;
        f.session.onMessage(std::string("\x07\x00", 2), f.now); // Close+junk.
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
        EXPECT(f.host.closes == std::vector<int>{1002});
    }
    {
        Fixture f;
        f.hello();
        f.session.onMessage(std::string(maxMessageSize + 1, '\x02'), f.now);
        EXPECT(f.host.lastIsError(ErrorCode::LimitExceeded));
        EXPECT(f.host.closes == std::vector<int>{1009});
        EXPECT_EQ(f.host.terminations, 1);
    }
    {
        Fixture f;
        f.hello();
        f.session.onMessage(std::string("\x82\x00", 2), f.now); // Server type.
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
    }
    {
        Fixture f;
        f.hello();
        f.session.onMessage(std::string("\x06", 1), f.now); // Reserved Detach.
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
    }
}

TEST_CASE("Input is forwarded and an overflowing queue is fatal") {
    Fixture f;
    f.hello();
    f.send(Input{"ls\r"});
    f.send(Input{"\x1b[<0;5;3M"});
    EXPECT_EQ(f.host.input.size(), std::size_t(2));
    EXPECT(f.host.input[0] == "ls\r");
    EXPECT(f.host.input[1] == "\x1b[<0;5;3M");
    f.host.inputOk = false;
    f.send(Input{"x"});
    EXPECT(f.host.lastIsError(ErrorCode::LimitExceeded));
    EXPECT(f.host.closes == std::vector<int>{1009});
    EXPECT_EQ(f.host.terminations, 1);
}

TEST_CASE("Ping is answered with the same nonce") {
    Fixture f;
    f.hello();
    f.send(Ping{0xdeadbeef});
    EXPECT_EQ(std::get<Pong>(f.host.messages().back()).nonce, 0xdeadbeefu);
}

TEST_CASE("Output is split into offsets without gaps") {
    Fixture f;
    f.hello();
    const std::string big(maxOutputData * 2 + 10, 'x');
    f.session.onProcessOutput(big, f.now);
    const auto messages = f.host.messages();
    EXPECT_EQ(messages.size(), std::size_t(4)); // Welcome + 3 outputs.
    std::uint64_t expected = 0;
    for (std::size_t i = 1; i < messages.size(); ++i) {
        const auto &output = std::get<Output>(messages[i]);
        EXPECT(output.offset == expected);
        EXPECT(output.data.size() <= maxOutputData);
        expected += output.data.size();
    }
    EXPECT(expected == big.size());
    EXPECT(f.session.sentOffset() == big.size());
}

TEST_CASE("The output window limits how much the host may read") {
    SessionConfig config;
    config.outputWindow = 1000;
    Fixture f(config);
    f.hello();
    EXPECT_EQ(f.session.outputBudget(), std::size_t(1000));
    f.session.onProcessOutput(std::string(600, 'a'), f.now);
    EXPECT_EQ(f.session.outputBudget(), std::size_t(400));
    f.session.onProcessOutput(std::string(400, 'a'), f.now);
    EXPECT_EQ(f.session.outputBudget(), std::size_t(0));
    f.send(Ack{500});
    EXPECT_EQ(f.session.outputBudget(), std::size_t(500));
    f.send(Ack{1000});
    EXPECT_EQ(f.session.outputBudget(), std::size_t(1000));
}

TEST_CASE("Acknowledgements must be monotonic and not exceed what was sent") {
    {
        Fixture f;
        f.hello();
        f.session.onProcessOutput("hello", f.now);
        f.send(Ack{6});
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
    }
    {
        Fixture f;
        f.hello();
        f.session.onProcessOutput("hello", f.now);
        f.send(Ack{4});
        f.send(Ack{3});
        EXPECT(f.host.lastIsError(ErrorCode::BadMessage));
    }
    {
        Fixture f;
        f.hello();
        f.session.onProcessOutput("hello", f.now);
        f.send(Ack{5});
        f.send(Ack{5}); // Repeating is harmless.
        EXPECT(f.session.state() == ServerSession::State::Running);
        EXPECT(f.session.ackedOffset() == 5);
    }
}

TEST_CASE("A client that stops acknowledging a full window is disconnected") {
    SessionConfig config;
    config.outputWindow = 100;
    Fixture f(config);
    f.hello();
    f.session.onProcessOutput(std::string(100, 'a'), f.now);
    // Keep the connection otherwise alive with pings.
    for (int i = 0; i < 29; ++i) {
        f.advance(1000);
        f.send(Ping{1});
    }
    EXPECT(f.session.state() == ServerSession::State::Running);
    f.advance(1000);
    EXPECT(f.host.lastIsError(ErrorCode::Timeout));
    EXPECT_EQ(f.host.terminations, 1);
}

TEST_CASE("Acknowledging in time clears the stall") {
    SessionConfig config;
    config.outputWindow = 100;
    Fixture f(config);
    f.hello();
    f.session.onProcessOutput(std::string(100, 'a'), f.now);
    f.advance(20000);
    f.send(Ack{50});
    f.advance(20000);
    f.send(Ping{1});
    EXPECT(f.session.state() == ServerSession::State::Running);
}

TEST_CASE("Idle connections time out and any message keeps them alive") {
    Fixture f;
    f.hello();
    f.advance(59000);
    f.send(Ping{1});
    f.advance(59000);
    EXPECT(f.session.state() == ServerSession::State::Running);
    EXPECT(f.session.nextDeadline() == f.now + milliseconds(1000));
    f.advance(1000);
    EXPECT(f.host.lastIsError(ErrorCode::Timeout));
    EXPECT(f.host.closes == std::vector<int>{1008});
}

TEST_CASE("Resizes apply at once, then coalesce to the latest one") {
    Fixture f;
    f.hello();
    f.advance(200);
    f.send(Resize{100, 30, 800, 600});
    EXPECT_EQ(f.host.resizes.size(), std::size_t(1));
    EXPECT_EQ(f.host.resizes[0].columns, 100);
    EXPECT_EQ(f.host.resizes[0].pixelWidth, 800);
    f.advance(10);
    f.send(Resize{101, 31, 0, 0});
    f.advance(10);
    f.send(Resize{102, 32, 0, 0});
    EXPECT_EQ(f.host.resizes.size(), std::size_t(1)); // Coalescing.
    EXPECT(f.session.nextDeadline().has_value());
    f.advance(80);
    EXPECT_EQ(f.host.resizes.size(), std::size_t(2));
    EXPECT_EQ(f.host.resizes[1].columns, 102);
    EXPECT_EQ(f.host.resizes[1].rows, 32);
}

TEST_CASE("Too many resizes in one second are rate limited") {
    Fixture f;
    f.hello();
    for (int i = 0; i < 100; ++i) {
        f.send(Resize{80, 24, 0, 0});
    }
    EXPECT(f.session.state() == ServerSession::State::Running);
    f.send(Resize{80, 24, 0, 0});
    EXPECT(f.host.lastIsError(ErrorCode::RateLimited));
    EXPECT(f.host.closes == std::vector<int>{1008});
}

TEST_CASE("A message flood is rate limited, a steady rate is not") {
    SessionConfig config;
    config.maxMessagesPerWindow = 50;
    {
        Fixture f(config);
        f.hello();
        for (int i = 0; i < 48; ++i) {
            f.send(Ping{1});
        }
        EXPECT(f.session.state() == ServerSession::State::Running);
        f.send(Ping{1});
        f.send(Ping{1});
        EXPECT(f.host.lastIsError(ErrorCode::RateLimited));
    }
    {
        Fixture f(config);
        f.hello();
        for (int window = 0; window < 3; ++window) {
            for (int i = 0; i < 40; ++i) {
                f.send(Ping{1});
            }
            f.advance(5000);
        }
        EXPECT(f.session.state() == ServerSession::State::Running);
    }
}

TEST_CASE("Process exit sends Exit last and closes normally") {
    Fixture f;
    f.hello();
    f.session.onProcessOutput("bye\r\n", f.now);
    f.session.onProcessExit(3, 0, f.now);
    const auto messages = f.host.messages();
    EXPECT(std::holds_alternative<Exit>(messages.back()));
    EXPECT_EQ(std::get<Exit>(messages.back()).code, 3);
    EXPECT(f.host.closes == std::vector<int>{1000});
    EXPECT(f.session.state() == ServerSession::State::Closed);
    EXPECT_EQ(f.host.terminations, 0);
    f.send(Ping{1}); // Ignored after close.
    EXPECT_EQ(f.host.messages().size(), messages.size());
}

TEST_CASE("Close terminates the process, then Exit completes the session") {
    Fixture f;
    f.hello();
    f.send(Close{});
    EXPECT(f.session.state() == ServerSession::State::Closing);
    EXPECT_EQ(f.host.terminations, 1);
    f.send(Input{"late"}); // Ignored.
    EXPECT(f.host.input.empty());
    f.session.onProcessOutput("tail", f.now); // Still delivered.
    f.session.onProcessExit(-1, 1, f.now);
    const auto messages = f.host.messages();
    EXPECT(std::get<Output>(messages[1]).data == "tail");
    EXPECT_EQ(std::get<Exit>(messages.back()).signal, 1);
    EXPECT(f.host.closes == std::vector<int>{1000});
}

TEST_CASE("A dropped transport terminates the process once") {
    Fixture f;
    f.hello();
    f.session.onTransportClosed(f.now);
    EXPECT_EQ(f.host.terminations, 1);
    EXPECT(f.session.state() == ServerSession::State::Closed);
    f.session.onTransportClosed(f.now);
    f.session.onProcessExit(1, 0, f.now); // Exit after close: nothing to send.
    EXPECT_EQ(f.host.terminations, 1);
    EXPECT(f.host.closes.empty());
    EXPECT_EQ(f.host.messages().size(), std::size_t(1)); // Just Welcome.
}

TEST_CASE("Shutdown tells the client and stops the process") {
    Fixture f;
    f.hello();
    f.session.shutdown(f.now);
    EXPECT(f.host.lastIsError(ErrorCode::ShuttingDown));
    EXPECT(f.host.closes == std::vector<int>{1001});
    EXPECT_EQ(f.host.terminations, 1);
}

TEST_CASE("Failure after the process exited does not terminate it again") {
    Fixture f;
    f.hello();
    f.session.onProcessExit(0, 0, f.now);
    f.send(Ping{1});
    EXPECT_EQ(f.host.terminations, 0);
}

TEST_SUIT_END
