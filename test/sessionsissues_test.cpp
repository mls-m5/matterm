// Known gaps in the server sessions found by the review
// (docs/SESSIONS_REVIEW.md). Each suite fails today and is registered as an
// expected failure (WILL_FAIL); when the code is fixed the test turns red: move
// it into sessionsreview_test.cpp and delete its entry in test/CMakeLists.txt.
#include "mls-unit-test/unittest.h"
#include "sessionregistry.h"
#include <thread>

namespace {
protocol::Time now() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}
std::string wire(const protocol::ClientMessage &message) {
    std::string result;
    protocol::encode(message, result);
    return result;
}
protocol::Hello hello(std::string id = {}, std::uint64_t offset = 0) {
    return {1, 80, 24, offset, id, protocol::featureReplay};
}
server::ProcessConfig process(
    std::string command = "printf ready; exec /bin/sh") {
    server::ProcessConfig result;
    result.arguments = {"/bin/sh", "-c", command};
    result.terminationGrace = std::chrono::milliseconds(0);
    result.terminationDrainTimeout = std::chrono::milliseconds(100);
    return result;
}
server::RegistryConfig persistentConfig() {
    server::RegistryConfig result;
    result.persistentSessions = true;
    result.replayBufferBytes = 4096;
    result.replayTotalBytes = 8192;
    return result;
}
bool until(server::SessionRegistry &registry,
           const std::function<bool()> &condition,
           int milliseconds = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    do {
        registry.tick(now());
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
// A stand-in for one HTTP connection: holds at most one login lease, which it
// gives back when the registry closes or releases it (as Connection does).
struct Leases {
    unsigned held = 0, limit = 3;
};
struct Conn {
    Leases *leases;
    std::vector<protocol::ServerMessage> messages;
    unsigned closes = 0;
    bool lease = false, detached = false, refuseAdmit = false;
    std::optional<server::SessionRegistry::Attachment> attachment;
    explicit Conn(Leases &l)
        : leases(&l) {}
    void give() {
        if (lease) {
            lease = false;
            --leases->held;
        }
    }
    server::SessionRegistry::Transport transport() {
        server::SessionRegistry::Transport t;
        t.send = [this](std::string bytes) {
            if (const auto decoded = protocol::decodeServer(bytes))
                messages.push_back(*decoded.message);
        };
        t.close = [this](int) {
            ++closes;
            give();
        };
        t.admit = [this](bool transfer) {
            if (refuseAdmit || leases->held >= leases->limit + transfer)
                return false;
            if (!lease) {
                lease = true;
                ++leases->held;
            }
            return true;
        };
        t.release = [this] { give(); };
        return t;
    }
    std::string output() const {
        std::string result;
        for (const auto &m : messages)
            if (const auto *out = std::get_if<protocol::Output>(&m))
                result += out->data;
        return result;
    }
    std::optional<protocol::Error> error() const {
        for (const auto &m : messages)
            if (const auto *e = std::get_if<protocol::Error>(&m))
                return *e;
        return {};
    }
};
bool seen(server::SessionRegistry &registry, Conn &conn, std::string what) {
    return until(registry,
                 [&] { return conn.output().find(what) != std::string::npos; });
}
} // namespace

TEST_SUIT_BEGIN(SessionsIssueRefusedResize)
TEST_CASE("A refused attach leaves the terminal size of a live session alone") {
    server::SessionRegistry registry(persistentConfig(), process(), {});
    Leases leases;
    Conn owner(leases), refused(leases);
    owner.attachment = registry.attach(hello(), owner.transport(), now());
    EXPECT(owner.attachment.has_value());
    EXPECT(seen(registry, owner, "ready"));
    owner.messages.clear();
    registry.onMessage(
        *owner.attachment, wire(protocol::Input{"stty size\n"}), now());
    EXPECT(seen(registry, owner, "24 80"));
    // The upgrade lease is refused (limit reached elsewhere), the Hello asks
    // for 50x200.
    refused.refuseAdmit = true;
    auto other = hello(owner.attachment->id);
    other.columns = 200;
    other.rows = 50;
    EXPECT_FALSE(
        registry.attach(other, refused.transport(), now()).has_value());
    EXPECT_EQ(owner.closes, 0u);
    owner.messages.clear();
    registry.onMessage(
        *owner.attachment, wire(protocol::Input{"stty size\n"}), now());
    EXPECT(seen(registry, owner, "24 80"));
    EXPECT_EQ(owner.output().find("50 200"), std::string::npos);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_SUIT_END
