#include "mls-unit-test/unittest.h"
#include "sessionregistry.h"
#include <thread>

namespace {
protocol::Time now() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}
struct Sink {
    std::vector<protocol::ServerMessage> messages;
    unsigned closes = 0;
    server::SessionRegistry::Transport transport() {
        return {[this](std::string bytes) {
                    const auto decoded = protocol::decodeServer(bytes);
                    if (decoded)
                        messages.push_back(*decoded.message);
                },
                [this](int) { ++closes; }};
    }
    std::string output() const {
        std::string result;
        for (const auto &m : messages)
            if (auto *out = std::get_if<protocol::Output>(&m))
                result += out->data;
        return result;
    }
    std::uint64_t end() const {
        std::uint64_t result = 0;
        for (const auto &m : messages)
            if (auto *out = std::get_if<protocol::Output>(&m))
                result = out->offset + out->data.size();
        return result;
    }
};
std::string wire(const protocol::ClientMessage &message) {
    std::string result;
    protocol::encode(message, result);
    return result;
}
bool until(server::SessionRegistry &registry,
           const std::function<bool()> &condition) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        registry.tick(now());
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
server::ProcessConfig process(
    std::string command = "printf ready; exec /bin/sh") {
    server::ProcessConfig result;
    result.arguments = {"/bin/sh", "-c", command};
    result.terminationGrace = std::chrono::milliseconds(0);
    result.terminationDrainTimeout = std::chrono::milliseconds(100);
    return result;
}
protocol::Hello hello(std::string id = {}, std::uint64_t offset = 0) {
    return {1, 80, 24, offset, id, 1};
}
server::RegistryConfig persistent() {
    server::RegistryConfig result;
    result.persistentSessions = true;
    result.replayBufferBytes = 4096;
    result.replayTotalBytes = 8192;
    return result;
}
} // namespace
TEST_SUIT_BEGIN(ServerRegistry)
TEST_CASE("Optional tick metrics count fresh PTY bytes once, never replay") {
    server::SessionRegistry registry(
        persistent(), process("printf ready; exec sleep 30"), {});
    Sink first, second;
    auto a = registry.attach(hello(), first.transport(), now());
    EXPECT(a.has_value());
    std::uint64_t bytes = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (first.output() != "ready" &&
           std::chrono::steady_clock::now() < deadline) {
        server::SessionRegistry::TickMetrics metrics{999, 999};
        registry.tick(now(), &metrics);
        bytes += metrics.ptyBytes;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT(first.output() == "ready");
    EXPECT(bytes == 5u);
    registry.detach(*a, now());
    auto b = registry.attach(hello(a->id), second.transport(), now());
    EXPECT(b.has_value());
    server::SessionRegistry::TickMetrics replay{999, 999};
    registry.tick(now(), &replay);
    EXPECT(second.output() == "ready");
    EXPECT(replay.ptyBytes == 0u);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_CASE("Tick metrics sample exhausted output windows without claiming "
          "pending PTY data") {
    protocol::SessionConfig config;
    config.outputWindow = 5;
    server::SessionRegistry registry(
        persistent(), process("printf ready; exec sleep 30"), config);
    Sink sink;
    auto a = registry.attach(hello(), sink.transport(), now());
    EXPECT(a.has_value());
    EXPECT(until(registry, [&] { return sink.output() == "ready"; }));
    server::SessionRegistry::TickMetrics metrics;
    registry.tick(now(), &metrics);
    EXPECT(metrics.ptyBytes == 0u);
    EXPECT(metrics.windowBlockedSessions == 1u);
    registry.onMessage(*a, wire(protocol::Ack{5}), now());
    registry.tick(now(), &metrics);
    EXPECT(metrics.windowBlockedSessions == 0u);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_CASE("Detached timeout zero is unlimited and only live attachment timers "
          "remain") {
    auto config = persistent();
    config.detachedTimeout = protocol::Time(0);
    server::SessionRegistry registry(config, process(), {});
    Sink sink;
    const auto at = now();
    auto a = registry.attach(hello(), sink.transport(), at);
    EXPECT(a.has_value());
    EXPECT(registry.nextDeadline().has_value());
    registry.detach(*a, at);
    EXPECT_FALSE(registry.nextDeadline().has_value());
    registry.tick(at + std::chrono::hours(2));
    EXPECT_EQ(registry.detachedSize(), std::size_t(1));
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_CASE("Total replay cap bounds multiple detached flooding PTYs") {
    auto config = persistent();
    config.replayTotalBytes = config.replayBufferBytes;
    server::SessionRegistry registry(
        config, process("exec yes 'replay line'"), {});
    Sink first, second;
    auto a = registry.attach(hello(), first.transport(), now());
    auto b = registry.attach(hello(), second.transport(), now());
    EXPECT(a && b);
    registry.detach(*a, now());
    registry.detach(*b, now());
    EXPECT(until(registry, [&] { return registry.replayCost() > 0; }));
    for (unsigned i = 0; i < 30; ++i) {
        registry.tick(now());
        EXPECT(registry.replayCost() <= config.replayTotalBytes);
    }
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
}
TEST_CASE("Detached shell resumes with the same id and safe replay, then "
          "takeover fences old events") {
    server::SessionRegistry registry(persistent(), process(), {});
    Sink first, second, third;
    auto a = registry.attach(hello(), first.transport(), now());
    EXPECT(a.has_value());
    EXPECT_EQ(a->id.size(), std::size_t(32));
    EXPECT(until(registry, [&] {
        return first.output().find("ready") != std::string::npos;
    }));
    registry.detach(*a, now());
    EXPECT_EQ(registry.detachedSize(), std::size_t(1));
    auto b = registry.attach(hello(a->id), second.transport(), now());
    EXPECT(b.has_value());
    EXPECT_EQ(b->id, a->id);
    EXPECT(until(registry, [&] {
        return second.output().find("ready") != std::string::npos;
    }));
    auto c =
        registry.attach(hello(a->id, second.end()), third.transport(), now());
    EXPECT(c.has_value());
    EXPECT_EQ(second.closes, 1u);
    EXPECT(std::holds_alternative<protocol::Error>(second.messages.back()));
    EXPECT(std::get<protocol::Error>(second.messages.back()).code ==
           protocol::ErrorCode::Replaced);
    registry.onMessage(*b, wire(protocol::Close{}), now());
    registry.detach(*b, now());
    registry.onMessage(*c, wire(protocol::Input{"echo survived\n"}), now());
    EXPECT(until(registry, [&] {
        return third.output().find("survived") != std::string::npos;
    }));
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
}
TEST_CASE("Zero buffer, disabled persistence and feature-zero terminals end "
          "with their connection") {
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto config = persistent();
        if (mode == 0)
            config.replayBufferBytes = 0;
        if (mode == 1)
            config.persistentSessions = false;
        server::SessionRegistry registry(config, process(), {});
        Sink sink;
        auto h = hello();
        if (mode == 2)
            h.features = 0;
        auto a = registry.attach(h, sink.transport(), now());
        EXPECT(a.has_value());
        registry.detach(*a, now());
        EXPECT(until(registry, [&] { return registry.size() == 0; }));
        EXPECT_EQ(registry.replayCost(), std::size_t(0));
    }
}
TEST_CASE("Guessed ids and invalid Hello never evict a valid attachment") {
    server::SessionRegistry registry(persistent(), process(), {});
    Sink first, bad;
    auto a = registry.attach(hello(), first.transport(), now());
    EXPECT(a.has_value());
    EXPECT_FALSE(
        registry.attach(hello(std::string(32, '0')), bad.transport(), now())
            .has_value());
    auto invalid = hello(a->id);
    invalid.version = 0;
    EXPECT_FALSE(registry.attach(invalid, bad.transport(), now()).has_value());
    EXPECT_EQ(first.closes, 0u);
    registry.onMessage(*a, wire(protocol::Input{"echo alive\n"}), now());
    EXPECT(until(registry, [&] {
        return first.output().find("alive") != std::string::npos;
    }));
}
TEST_CASE(
    "Limits and detached timeout bound living sessions and replay memory") {
    auto config = persistent();
    config.maxSessions = 2;
    config.maxDetachedSessions = 1;
    config.detachedTimeout = protocol::Time(50);
    server::SessionRegistry registry(
        config, process("exec yes 'line of replay'"), {});
    Sink aSink, bSink, rejected;
    auto a = registry.attach(hello(), aSink.transport(), now());
    auto b = registry.attach(hello(), bSink.transport(), now());
    EXPECT(a && b);
    EXPECT_FALSE(
        registry.attach(hello(), rejected.transport(), now()).has_value());
    registry.detach(*a, now());
    registry.detach(*b,
                    now()); // Limit reached: terminate this one, not the first.
    EXPECT(registry.detachedSize() <= 1);
    for (unsigned i = 0; i < 20; ++i) {
        registry.tick(now());
        EXPECT(registry.replayCost() <= config.replayTotalBytes);
    }
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
}
TEST_CASE("Exit while detached wipes output and leaves only a bounded status "
          "tombstone") {
    server::SessionRegistry registry(
        persistent(), process("printf ending; sleep 0.03; exit 7"), {});
    Sink first, resumed;
    auto a = registry.attach(hello(), first.transport(), now());
    EXPECT(a.has_value());
    registry.detach(*a, now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
    EXPECT_EQ(registry.tombstoneSize(), std::size_t(1));
    EXPECT(
        registry.attach(hello(a->id), resumed.transport(), now()).has_value());
    EXPECT_EQ(std::get<protocol::Exit>(resumed.messages.back()).code, 7);
    EXPECT(resumed.output().empty());
}
TEST_CASE(
    "A replay is sent up to the window at once, not one message per pump") {
    auto config = persistent();
    config.replayBufferBytes = 1 << 20;
    config.replayTotalBytes = 2 << 20;
    // About 600 KB of output, then it waits.
    server::SessionRegistry registry(
        config,
        process("head -c 600000 /dev/zero | tr '\\0' x; echo done; sleep 30"),
        {});
    Sink first, second;
    auto a = registry.attach(hello(), first.transport(), now());
    EXPECT(a.has_value());
    EXPECT(until(registry, [&] {
        registry.onMessage(
            *a, wire(protocol::Ack{first.end()}), now()); // As a client does.
        return first.output().find("done") != std::string::npos;
    }));
    registry.onMessage(*a, wire(protocol::Ack{first.end()}), now());
    registry.detach(*a, now());
    auto b = registry.attach(hello(a->id, 0), second.transport(), now());
    EXPECT(b.has_value());
    // Two pump ticks: the Welcome, then the replay. The window is 256 KiB: most
    // of it goes out in the first tick that has room, where it used to be 16
    // KiB per tick.
    registry.tick(now());
    registry.tick(now());
    EXPECT(second.output().size() > 200000u);
    EXPECT(second.output().size() <= 262144u + 16384u);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}

TEST_SUIT_END
