#include "clientsession.h"
#include "mls-unit-test/unittest.h"
#include "serversession.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

namespace {
using namespace protocol;
constexpr unsigned seed = 0x5eed7101;
std::string hex(std::string_view bytes) {
    std::ostringstream out;
    for (unsigned char c : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
bool check(bool condition, unsigned trial, std::string_view bytes) {
    if (!condition)
        std::cerr << "seed=" << seed << " trial=" << trial
                  << " input(hex)=" << hex(bytes) << '\n';
    return condition;
}
template <class Message>
std::string wire(const Message &message) {
    std::string bytes;
    if (!encode(message, bytes))
        throw std::runtime_error("invalid fuzz fixture");
    return bytes;
}
std::string mutate(std::mt19937 &random, std::string bytes) {
    switch (random() % 4) {
    case 0:
        if (!bytes.empty())
            bytes[random() % bytes.size()] = static_cast<char>(random());
        break;
    case 1:
        bytes.resize(random() % (bytes.size() + 1));
        break;
    case 2:
        bytes.insert(
            random() % (bytes.size() + 1), 1, static_cast<char>(random()));
        break;
    case 3:
        bytes.assign(random() % 128, '\0');
        for (auto &c : bytes)
            c = static_cast<char>(random());
        break;
    }
    return bytes;
}
struct Host : SessionHost {
    std::vector<std::string> messages;
    unsigned exits = 0, closes = 0;
    bool inputOk = true;
    void send(std::string bytes) override {
        if (auto decoded = decodeServer(bytes);
            decoded && std::holds_alternative<Exit>(*decoded.message))
            ++exits;
        messages.push_back(std::move(bytes));
    }
    void closeTransport(int) override {
        ++closes;
    }
    bool startProcess(std::uint16_t, std::uint16_t) override {
        return true;
    }
    bool writeInput(std::string_view) override {
        return inputOk;
    }
    void resizeProcess(std::uint16_t,
                       std::uint16_t,
                       std::uint16_t,
                       std::uint16_t) override {}
    void terminateProcess() override {}
};
struct Channel : MessageChannel {
    Events events;
    unsigned sent = 0, closes = 0;
    std::size_t largest = 0;
    void setEvents(Events value) override {
        events = std::move(value);
    }
    bool send(std::string_view bytes) override {
        ++sent;
        largest = std::max(largest, bytes.size());
        return true;
    }
    void close(int) override {
        ++closes;
    }
};
} // namespace
TEST_SUIT_BEGIN(ProtocolFuzz)
TEST_CASE(
    "Seeded mutations round-trip every accepted client and server message") {
    std::vector<std::string> corpus{
        wire(ClientMessage{Hello{}}),
        wire(ClientMessage{
            Hello{1, 80, 24, 1234, "00112233445566778899aabbccddeeff", 1}}),
        wire(ServerMessage{
            Welcome{1, "00112233445566778899aabbccddeeff", 2048, 3, 1}}),
        wire(ClientMessage{Input{std::string("\x00\xff\x1b", 3)}}),
        wire(ClientMessage{Resize{1000, 1, 65535, 65535}}),
        wire(ClientMessage{Ack{~std::uint64_t(0)}}),
        wire(ClientMessage{Ping{0xfeedbeef}}),
        wire(ClientMessage{Close{}}),
        wire(ServerMessage{Welcome{1, "fuzz", 0, 0, 0}}),
        wire(ServerMessage{Output{0, "abc"}}),
        wire(ServerMessage{Exit{-1, 9}}),
        wire(ServerMessage{Pong{123}}),
        wire(ServerMessage{Error{ErrorCode::BadMessage, "\xc3\xa5"}}),
        wire(ClientMessage{Input{std::string(maxInputData, 'x')}}),
        wire(ServerMessage{Output{42, std::string(maxOutputData, '\xff')}}),
        std::string(maxMessageSize + 1, 'x')};
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 6000; ++trial) {
        auto bytes = corpus[random() % corpus.size()];
        if (trial >= corpus.size())
            bytes = mutate(random, std::move(bytes));
        if (auto decoded = decodeClient(bytes)) {
            EXPECT(check(wire(*decoded.message) == bytes, trial, bytes));
            EXPECT(check(!decodeServer(bytes), trial, bytes));
        }
        if (auto decoded = decodeServer(bytes)) {
            EXPECT(check(wire(*decoded.message) == bytes, trial, bytes));
            EXPECT(check(!decodeClient(bytes), trial, bytes));
        }
    }
}
TEST_CASE("Seeded server schedules preserve flow control and terminal states") {
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 250; ++trial) {
        Host host;
        SessionConfig config;
        config.outputWindow = 256;
        ServerSession session(config, host);
        Time now{0};
        session.start(now);
        auto hello = wire(ClientMessage{Hello{}});
        if (trial % 5 == 0)
            hello = mutate(random, std::move(hello));
        session.onMessage(hello, now);
        std::string trace = hello;
        for (unsigned step = 0; step < 80; ++step) {
            const auto before = session.state();
            const auto sentBefore = host.messages.size();
            const auto op = random() % 10;
            trace += static_cast<char>(op);
            now += Time(random() % 101);
            if (trial % 7 == 0 && step == 45)
                now += Time(100000);
            std::string bytes;
            switch (op) {
            case 0:
                bytes = wire(ClientMessage{Input{"paste"}});
                break;
            case 1:
                bytes = wire(ClientMessage{
                    Resize{std::uint16_t(1 + random() % 1000), 24, 0, 0}});
                break;
            case 2:
                bytes = wire(ClientMessage{Ack{session.sentOffset()}});
                break;
            case 3:
                bytes = wire(ClientMessage{
                    Ack{session.sentOffset() + (step > 40 ? 1u : 0u)}});
                break;
            case 4:
                bytes = wire(
                    ClientMessage{Ping{static_cast<std::uint32_t>(random())}});
                break;
            case 5:
                bytes.assign(std::min<std::size_t>(session.outputBudget(),
                                                   random() % 128),
                             'x');
                session.onProcessOutput(bytes, now);
                bytes.clear();
                break;
            case 6:
                if (step > 60)
                    session.onProcessExit(0, 0, now);
                break;
            case 7:
                if (step > 60)
                    bytes = wire(ClientMessage{Close{}});
                break;
            case 8:
                if (step > 60)
                    session.onTransportClosed(now);
                break;
            case 9:
                if (step > 60)
                    host.inputOk = false;
                if (trial % 3 == 0 && step > 45)
                    bytes = mutate(random, wire(ClientMessage{Resize{}}));
                break;
            }
            trace += wire(
                ClientMessage{Ping{static_cast<std::uint32_t>(now.count())}});
            trace += bytes;
            if (!bytes.empty())
                session.onMessage(bytes, now);
            session.tick(now);
            EXPECT(check(
                session.ackedOffset() <= session.sentOffset(), trial, trace));
            EXPECT(check(session.sentOffset() - session.ackedOffset() <=
                             config.outputWindow,
                         trial,
                         trace));
            EXPECT(check(host.exits <= 1 && host.closes <= 1, trial, trace));
            if (before == ServerSession::State::Closed) {
                EXPECT(check(session.state() == before &&
                                 host.messages.size() == sentBefore,
                             trial,
                             trace));
            }
        }
        // EOF after a bounded host termination/drain cycle; Closing itself
        // intentionally depends on the host reporting exit, not a parser timer.
        session.onProcessExit(7, 0, now);
        session.shutdown(now);
        EXPECT(check(session.state() == ServerSession::State::Closed &&
                         host.exits <= 1,
                     trial,
                     trace));
        const auto count = host.messages.size();
        session.onMessage(wire(ClientMessage{Hello{}}), now);
        session.onProcessOutput("late", now);
        session.onProcessExit(0, 0, now);
        session.tick(now + Time(100000));
        EXPECT(check(host.messages.size() == count, trial, trace));
    }
}
TEST_CASE("Hostile server streams fail closed without accumulating output") {
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 250; ++trial) {
        Channel channel;
        Time now{0};
        ClientSession::Config config;
        config.clock = [&] { return now; };
        ClientSession session(channel, config);
        std::size_t outputBytes = 0;
        unsigned exits = 0, failures = 0;
        session.setEvents(
            {[&](std::string_view bytes) { outputBytes += bytes.size(); },
             [&](int, int) { ++exits; },
             [&](std::string_view) { ++failures; }});
        channel.events.opened();
        if (trial % 2)
            channel.events.message(wire(ServerMessage{Welcome{}}));
        std::string trace;
        for (unsigned step = 0; step < 40; ++step) {
            auto before = session.state();
            const auto sentBefore = channel.sent;
            const auto outputsBefore = outputBytes;
            const auto choice = random() % 6;
            std::string bytes;
            switch (choice) {
            case 0:
                bytes = wire(
                    ServerMessage{Output{session.receivedOffset(), "output"}});
                break;
            case 1:
                bytes = wire(
                    ServerMessage{Output{session.receivedOffset() + 1, "gap"}});
                break;
            case 2:
                bytes = wire(ServerMessage{Welcome{}});
                break;
            case 3:
                bytes = wire(ServerMessage{Exit{7, 0}});
                break;
            case 4:
                bytes = wire(
                    ServerMessage{Pong{static_cast<std::uint32_t>(random())}});
                break;
            case 5:
                bytes = mutate(random, wire(ServerMessage{Output{0, "fuzz"}}));
                break;
            }
            trace += bytes;
            channel.events.message(bytes);
            const bool live = before == ClientSession::State::Running ||
                              before == ClientSession::State::AwaitWelcome;
            const bool knownInvalid =
                choice == 1 ||
                (choice == 0 && before == ClientSession::State::AwaitWelcome) ||
                (choice == 2 && before == ClientSession::State::Running) ||
                ((choice == 3 || choice == 4) &&
                 before == ClientSession::State::AwaitWelcome);
            if (live && knownInvalid) {
                EXPECT(check(session.state() == ClientSession::State::Failed &&
                                 outputBytes == outputsBefore,
                             trial,
                             trace));
            }
            now += Time(20);
            session.tick();
            EXPECT(
                check(session.receivedOffset() == outputBytes, trial, trace));
            EXPECT(check(session.sessionId().size() <= maxSessionIdSize &&
                             channel.largest <= maxMessageSize,
                         trial,
                         trace));
            EXPECT(check(
                exits + failures <= 1 && channel.closes <= 1, trial, trace));
            if (before == ClientSession::State::Failed ||
                before == ClientSession::State::Exited) {
                EXPECT(check(session.state() == before &&
                                 channel.sent == sentBefore &&
                                 outputBytes == outputsBefore,
                             trial,
                             trace));
            }
        }
        channel.events.message("\xff");
        EXPECT(check(session.state() == ClientSession::State::Failed ||
                         session.state() == ClientSession::State::Exited,
                     trial,
                     trace));
    }
}
TEST_CASE("Seeded replay negotiation rejects hostile flags and keeps absolute "
          "counters bounded") {
    std::mt19937 random(seed);
    for (unsigned trial = 0; trial < 180; ++trial) {
        Channel channel;
        Time now{0};
        ClientSession::Config config;
        config.clock = [&] { return now; };
        config.replay = true;
        config.sessionId = "00112233445566778899aabbccddeeff";
        config.resumeOffset = 1 + random() % 4096;
        unsigned resets = 0;
        config.resetForReplay = [&] { ++resets; };
        ClientSession session(channel, config);
        std::size_t delivered = 0;
        session.setEvents(
            {[&](std::string_view bytes) { delivered += bytes.size(); },
             {},
             {}});
        channel.events.opened();
        Welcome welcome{
            1, config.sessionId, config.resumeOffset, 1, featureReplay};
        if (trial % 2) {
            welcome.flags = 3;
            welcome.startOffset =
                trial % 13 == 0 ? ~std::uint64_t(0) - 2 : 2048;
        }
        if (trial % 5 == 0)
            welcome.flags = 0x80;
        if (trial % 7 == 0)
            welcome.features = 3;
        if (trial % 11 == 0)
            welcome.sessionId = "wrong";
        std::string trace = wire(ServerMessage{welcome});
        channel.events.message(trace);
        const bool invalid = welcome.flags == 0x80 || welcome.features == 3 ||
                             welcome.sessionId != config.sessionId;
        EXPECT(check(invalid ? session.state() == ClientSession::State::Failed
                             : session.state() == ClientSession::State::Running,
                     trial,
                     trace));
        EXPECT(check(
            resets == unsigned(!invalid && welcome.flags == 3), trial, trace));
        for (unsigned step = 0; step < 12; ++step) {
            const auto before = session.state();
            const auto offset = session.receivedOffset();
            const auto oldDelivered = delivered;
            const std::string data(1 + random() % 8, 'x');
            const bool gap = step == 8;
            const auto sentOffset = gap ? (offset ? offset - 1 : 1) : offset;
            const auto bytes = wire(ServerMessage{Output{sentOffset, data}});
            trace += bytes;
            channel.events.message(bytes);
            now += Time(20);
            session.tick();
            const bool mustFail = before == ClientSession::State::Failed ||
                                  gap ||
                                  data.size() > ~std::uint64_t(0) - offset;
            if (mustFail) {
                EXPECT(check(session.state() == ClientSession::State::Failed &&
                                 delivered == oldDelivered &&
                                 session.receivedOffset() == offset,
                             trial,
                             trace));
            }
            else {
                EXPECT(
                    check(delivered == oldDelivered + data.size() &&
                              session.receivedOffset() == offset + data.size(),
                          trial,
                          trace));
            }
        }
    }
}
TEST_SUIT_END
