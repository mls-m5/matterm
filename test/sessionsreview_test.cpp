// Threat-model tests for the server sessions (docs/SESSIONS_REVIEW.md): the
// replay ring, the registry (attach, takeover, detach, close-all) and the
// login leases. Registry tests need no sockets; the last ones run a real
// server over loopback.
#include "config.h"
#include "mls-unit-test/unittest.h"
#include "replayring.h"
#include "serverfixture.h"
#include "sessionregistry.h"
#include <map>
#include <random>
#include <set>

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

TEST_SUIT_BEGIN(SessionsReview)
// ---- replay ring -----------------------------------------------------------
TEST_CASE("Ring against a model: contiguous tail, bounded cost, safe start "
          "after a newline") {
    std::mt19937_64 random(20261121);
    for (unsigned round = 0; round < 200; ++round) {
        const std::size_t budget = 200 + random() % 3000;
        server::ReplayRing ring(budget);
        std::string model;
        for (unsigned step = 0; step < 150; ++step) {
            std::string chunk;
            const auto kind = random() % 5;
            const std::size_t size = kind == 0   ? 1
                                     : kind == 1 ? random() % 8
                                     : kind == 2 ? random() % 400
                                     : kind == 3 ? random() % 6000
                                                 : 0;
            for (std::size_t i = 0; i < size; ++i)
                chunk += random() % 11 == 0 ? '\n' : char('a' + random() % 26);
            ring.append(protocol::Time(step), chunk, step);
            model += chunk;
            if (random() % 40 == 0)
                ring.dropOldest();
            EXPECT(ring.cost() <= budget);
            EXPECT_EQ(ring.end(), std::uint64_t(model.size()));
            EXPECT(ring.oldest() <= ring.end());
            const auto tail = ring.read(ring.oldest(), 1u << 20);
            EXPECT(tail.has_value());
            if (tail)
                EXPECT_EQ(*tail, model.substr(ring.oldest()));
            if (ring.oldest() > 0)
                EXPECT_FALSE(ring.read(ring.oldest() - 1, 4).has_value());
            EXPECT_FALSE(ring.read(ring.end() + 1, 4).has_value());
            const auto safe = ring.safeStart();
            EXPECT(safe >= ring.oldest() && safe <= ring.end());
            if (ring.oldest() == 0) {
                EXPECT_EQ(safe, std::uint64_t(0));
            }
            else if (safe > ring.oldest() && safe < ring.end()) {
                EXPECT_EQ(model[safe - 1], '\n');
            }
            // Reading in small pieces gives the same bytes.
            if (step % 25 == 0 && tail) {
                std::string pieces;
                auto at = ring.oldest();
                while (at < ring.end()) {
                    const auto piece = ring.read(at, 1 + random() % 97);
                    EXPECT(piece.has_value() && !piece->empty());
                    if (!piece || piece->empty())
                        break;
                    pieces += *piece;
                    at += piece->size();
                }
                EXPECT_EQ(pieces, *tail);
            }
        }
    }
}
TEST_CASE("One-byte writes cannot grow the ring past its budget (overhead is "
          "counted)") {
    server::ReplayRing ring(4096);
    for (unsigned i = 0; i < 200000; ++i)
        ring.append(protocol::Time(i), "x", i);
    EXPECT(ring.cost() <= 4096);
    EXPECT_EQ(ring.end(), std::uint64_t(200000));
    const auto tail = ring.read(ring.oldest(), 1u << 20);
    EXPECT(tail.has_value());
    EXPECT_EQ(tail->size(), std::size_t(ring.end() - ring.oldest()));
}
// ---- registry: hostile Hellos ----------------------------------------------
TEST_CASE("Hostile Hellos never evict, leak a lease, echo the id or disturb "
          "the sessions that exist") {
    auto config = persistentConfig();
    config.maxSessions = 3;
    config.maxDetachedSessions = 3;
    server::SessionRegistry registry(config, process(), {});
    Leases leases;
    leases.limit = 8;
    Conn live(leases), parked(leases);
    live.attachment = registry.attach(hello(), live.transport(), now());
    parked.attachment = registry.attach(hello(), parked.transport(), now());
    EXPECT(live.attachment && parked.attachment);
    EXPECT(seen(registry, live, "ready"));
    EXPECT(seen(registry, parked, "ready"));
    registry.detach(*parked.attachment, now());
    parked.detached = true;
    const std::string liveId = live.attachment->id,
                      parkedId = parked.attachment->id;
    const std::set<std::string> allowed = {"invalid Hello",
                                           "session unavailable",
                                           "invalid resume offset",
                                           "session limit",
                                           "upgrade limit",
                                           "could not attach"};
    std::mt19937_64 random(99);
    const auto mutate = [&](std::string id) {
        switch (random() % 9) {
        case 0:
            id[random() % 32] ^= 1; // Flip a bit: not hex, or other hex.
            break;
        case 1:
            for (auto &c : id)
                c = char(std::toupper(c));
            if (id == liveId || id == parkedId)
                id[0] = 'G';
            break;
        case 2:
            id.pop_back();
            break;
        case 3:
            id += '0';
            break;
        case 4:
            id[random() % 32] = '\0';
            break;
        case 5:
            id = std::string(32, '0');
            break;
        case 6:
            id = std::string(1 + random() % 200, char('a' + random() % 6));
            break;
        case 7:
            id = id.substr(0, 16) + std::string(16, 'f');
            break;
        default:
            id = "../../etc/passwd" + id.substr(0, 16);
            break;
        }
        return id == liveId || id == parkedId ? std::string("zz") : id;
    };
    const std::uint64_t offsets[] = {
        0, 1, 2, 4096, ~std::uint64_t(0), 1u << 31};
    unsigned refused = 0;
    for (unsigned i = 0; i < 3000; ++i) {
        Conn probe(leases);
        probe.refuseAdmit = random() % 4 == 0;
        auto h = hello(mutate(random() % 2 ? liveId : parkedId),
                       offsets[random() % 6]);
        if (random() % 7 == 0)
            h.version = std::uint16_t(random() % 4);
        if (random() % 9 == 0)
            h.columns = std::uint16_t(random() % 3 ? 0 : 65535);
        if (random() % 9 == 0)
            h.rows = std::uint16_t(random() % 3 ? 0 : 65535);
        if (random() % 5 == 0)
            h.features = std::uint32_t(random());
        if (random() % 6 == 0)
            h.sessionId.clear(); // A new session: refused only by the limit.
        const bool newSession = h.sessionId.empty();
        probe.attachment = registry.attach(h, probe.transport(), now());
        if (probe.attachment && newSession && !probe.detached) {
            // Legitimate (a free slot): give it back right away.
            registry.detach(*probe.attachment, now());
            continue;
        }
        EXPECT_FALSE(probe.attachment.has_value());
        ++refused;
        EXPECT(!probe.lease); // A refused attach holds no lease.
        EXPECT_EQ(live.closes, 0u);
        EXPECT(registry.detachedSize() >= 1);
        if (const auto e = probe.error()) {
            EXPECT(allowed.count(e->reason) == 1);
            EXPECT(e->reason.find(liveId.substr(0, 8)) == std::string::npos);
            EXPECT(e->reason.find(parkedId.substr(0, 8)) == std::string::npos);
        }
        // The same reason for every unavailable id, valid or not.
        EXPECT_EQ(leases.held, 1u);
    }
    EXPECT(refused > 1000);
    live.messages.clear();
    registry.onMessage(
        *live.attachment, wire(protocol::Input{"echo fine\n"}), now());
    EXPECT(seen(registry, live, "fine"));
    // The parked one still resumes with its own process.
    Conn back(leases);
    back.attachment = registry.attach(hello(parkedId), back.transport(), now());
    EXPECT(back.attachment.has_value());
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_CASE("A second Hello on an attached connection cannot reach another "
          "session") {
    server::SessionRegistry registry(persistentConfig(), process(), {});
    Leases leases;
    Conn a(leases), b(leases);
    a.attachment = registry.attach(hello(), a.transport(), now());
    b.attachment = registry.attach(hello(), b.transport(), now());
    EXPECT(a.attachment && b.attachment);
    registry.detach(*b.attachment, now());
    EXPECT_EQ(registry.detachedSize(), std::size_t(1));
    registry.onMessage(*a.attachment, wire(hello(b.attachment->id)), now());
    EXPECT_EQ(registry.detachedSize(), std::size_t(1));
    Conn again(leases);
    EXPECT(registry.attach(hello(b.attachment->id), again.transport(), now())
               .has_value());
    EXPECT_EQ(again.closes, 0u);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
// ---- registry: takeover fences --------------------------------------------
TEST_CASE("Every late event of a replaced attachment is ignored: input, "
          "resize, ack, detach, fail") {
    server::SessionRegistry registry(persistentConfig(), process(), {});
    Leases leases;
    Conn first(leases), second(leases);
    first.attachment = registry.attach(hello(), first.transport(), now());
    EXPECT(first.attachment.has_value());
    EXPECT(seen(registry, first, "ready"));
    const auto old = *first.attachment;
    second.attachment =
        registry.attach(hello(old.id, 0), second.transport(), now());
    EXPECT(second.attachment.has_value());
    EXPECT(seen(registry, second, "ready"));
    EXPECT_EQ(first.closes, 1u);
    EXPECT(first.error().has_value() &&
           first.error()->code == protocol::ErrorCode::Replaced);
    second.messages.clear();
    registry.onMessage(old, wire(protocol::Input{"echo STALEINPUT\n"}), now());
    registry.onMessage(old, wire(protocol::Resize{200, 50, 0, 0}), now());
    registry.onMessage(old, wire(protocol::Close{}), now());
    registry.onMessage(old, wire(protocol::Ack{1u << 30}), now());
    registry.detach(old, now());
    registry.fail(old, now());
    registry.detach(old, now());
    EXPECT_EQ(second.closes, 0u);
    EXPECT_EQ(registry.detachedSize(), std::size_t(0));
    registry.onMessage(*second.attachment,
                       wire(protocol::Input{"echo CURRENT; stty size\n"}),
                       now());
    EXPECT(seen(registry, second, "CURRENT"));
    EXPECT(seen(registry, second, "24 80"));
    EXPECT_EQ(second.output().find("STALEINPUT"), std::string::npos);
    // A third takeover, then the first generation again.
    Conn third(leases);
    third.attachment = registry.attach(hello(old.id), third.transport(), now());
    EXPECT(third.attachment.has_value());
    registry.detach(old, now());
    registry.detach(*second.attachment, now());
    EXPECT_EQ(registry.detachedSize(), std::size_t(0));
    EXPECT_EQ(third.closes, 0u);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
TEST_CASE("The detached timeout runs from the detachment and rejected "
          "attempts do not extend it") {
    auto config = persistentConfig();
    config.detachedTimeout = std::chrono::milliseconds(300);
    server::SessionRegistry registry(config, process(), {});
    Leases leases;
    Conn conn(leases);
    const auto start = now();
    conn.attachment = registry.attach(hello(), conn.transport(), start);
    EXPECT(conn.attachment.has_value());
    EXPECT(seen(registry, conn, "ready"));
    const auto id = conn.attachment->id;
    const auto parked = now();
    registry.detach(*conn.attachment, parked);
    for (int step = 1; step <= 4; ++step) {
        const auto at = parked + std::chrono::milliseconds(60 * step);
        Conn bad(leases);
        bad.refuseAdmit = step % 2 == 0;
        EXPECT_FALSE(registry.attach(hello(id, 1u << 20), bad.transport(), at)
                         .has_value());
        Conn denied(leases);
        denied.refuseAdmit = true;
        EXPECT_FALSE(
            registry.attach(hello(id), denied.transport(), at).has_value());
        registry.tick(at);
        EXPECT_EQ(registry.detachedSize(), std::size_t(1));
    }
    registry.tick(parked + std::chrono::milliseconds(301));
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    Conn late(leases);
    EXPECT_FALSE(
        registry.attach(hello(id), late.transport(), now()).has_value());
}
// ---- registry: exit status and close-all ----------------------------------
TEST_CASE("Exit tombstones are bounded, carry no output and die with "
          "close-all") {
    auto config = persistentConfig();
    config.maxSessions = 3;
    config.maxDetachedSessions = 3;
    server::SessionRegistry registry(
        config, process("printf SECRETOUTPUT; exit 5"), {});
    Leases leases;
    leases.limit = 100;
    std::vector<std::string> ids;
    for (unsigned i = 0; i < 9; ++i) {
        Conn conn(leases);
        std::optional<server::SessionRegistry::Attachment> a;
        for (unsigned tries = 0; tries < 400 && !a; ++tries) {
            a = registry.attach(hello(), conn.transport(), now());
            if (!a)
                until(
                    registry, [&] { return registry.size() < 3; }, 20);
        }
        EXPECT(a.has_value());
        if (!a)
            break;
        ids.push_back(a->id);
        registry.detach(*a, now());
        EXPECT(registry.tombstoneSize() <= 3);
    }
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT(registry.tombstoneSize() <= 3);
    EXPECT(registry.tombstoneSize() >= 1);
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
    Conn report(leases);
    const auto last =
        registry.attach(hello(ids.back()), report.transport(), now());
    EXPECT(last.has_value());
    EXPECT(report.output().empty());
    EXPECT(std::holds_alternative<protocol::Exit>(report.messages.back()));
    // The oldest tombstones were forgotten.
    Conn forgotten(leases);
    EXPECT_FALSE(
        registry.attach(hello(ids.front()), forgotten.transport(), now())
            .has_value());
    registry.closeAll(now());
    EXPECT_EQ(registry.tombstoneSize(), std::size_t(0));
    Conn gone(leases);
    EXPECT_FALSE(registry.attach(hello(ids.back()), gone.transport(), now())
                     .has_value());
}
TEST_CASE("Close-all closes every attachment, kills every shell, wipes the "
          "rings and forgets the ids") {
    server::SessionRegistry registry(persistentConfig(), process(), {});
    Leases leases;
    Conn live(leases), parked(leases);
    live.attachment = registry.attach(hello(), live.transport(), now());
    parked.attachment = registry.attach(hello(), parked.transport(), now());
    EXPECT(seen(registry, live, "ready"));
    EXPECT(seen(registry, parked, "ready"));
    const auto liveId = live.attachment->id, parkedId = parked.attachment->id;
    registry.detach(*parked.attachment, now());
    EXPECT(registry.replayCost() > 0);
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT(live.closes >= 1);
    EXPECT_EQ(registry.replayCost(), std::size_t(0));
    EXPECT_EQ(registry.tombstoneSize(), std::size_t(0));
    EXPECT_EQ(leases.held, 0u);
    for (const auto &id : {liveId, parkedId}) {
        Conn again(leases);
        EXPECT_FALSE(
            registry.attach(hello(id), again.transport(), now()).has_value());
    }
    // And the registry is usable afterwards.
    Conn fresh(leases);
    fresh.attachment = registry.attach(hello(), fresh.transport(), now());
    EXPECT(fresh.attachment.has_value());
    EXPECT(seen(registry, fresh, "ready"));
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
}
// ---- leases ---------------------------------------------------------------
TEST_CASE("Random attach, takeover, detach, fail and close-all keep the login "
          "leases balanced") {
    auto config = persistentConfig();
    config.maxSessions = 4;
    config.maxDetachedSessions = 2;
    server::SessionRegistry registry(config, process(), {});
    Leases leases;
    leases.limit = 3;
    std::mt19937_64 random(7);
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<std::string> ids;
    const auto check = [&] {
        EXPECT(leases.held <= leases.limit + 1);
        unsigned count = 0;
        for (const auto &c : conns) {
            count += c->lease;
            if (c->lease) {
                EXPECT_EQ(c->closes, 0u);
                EXPECT_FALSE(c->detached);
            }
        }
        EXPECT_EQ(count, leases.held);
        EXPECT(leases.held <= registry.size());
    };
    for (unsigned step = 0; step < 600; ++step) {
        switch (random() % 8) {
        case 0:
        case 1: {
            auto c = std::make_unique<Conn>(leases);
            const bool resume = !ids.empty() && random() % 2;
            c->attachment = registry.attach(
                hello(resume ? ids[random() % ids.size()] : std::string()),
                c->transport(),
                now());
            if (c->attachment)
                ids.push_back(c->attachment->id);
            conns.push_back(std::move(c));
            break;
        }
        case 2:
        case 3:
            if (!conns.empty()) {
                auto &c = *conns[random() % conns.size()];
                if (c.attachment && !c.detached) {
                    registry.detach(*c.attachment, now());
                    c.detached = true;
                    c.give(); // The connection releases its lease when it dies.
                }
            }
            break;
        case 4:
            if (!conns.empty()) {
                auto &c = *conns[random() % conns.size()];
                if (c.attachment && !c.detached)
                    registry.fail(*c.attachment, now());
            }
            break;
        case 5:
            if (random() % 6 == 0)
                registry.closeAll(now());
            break;
        default:
            registry.tick(now());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
        }
        check();
    }
    // Every connection goes away; nothing may keep a lease.
    for (auto &c : conns)
        if (c->attachment && !c->detached) {
            registry.detach(*c->attachment, now());
            c->detached = true;
            c->give();
        }
    registry.closeAll(now());
    EXPECT(until(registry, [&] { return registry.size() == 0; }));
    EXPECT_EQ(leases.held, 0u);
}
TEST_SUIT_END

// ---- over real sockets -----------------------------------------------------
namespace {
void persistent(server::ServerConfig &config) {
    server::Settings settings;
    settings.server = config;
    server::applyConfigText(
        R"({"persistent_sessions":true,"replay_buffer_bytes":65536,"replay_total_bytes":131072,"max_upgrades_per_login":8})",
        settings);
    config = settings.server;
    config.process.terminationGrace = std::chrono::milliseconds(0);
}
std::pair<std::string, int> startSession(Client &client) {
    client.send(protocol::Hello{1, 80, 24, 0, "", protocol::featureReplay});
    auto message = client.read();
    if (!std::holds_alternative<protocol::Welcome>(message))
        return {"", -1};
    return {std::get<protocol::Welcome>(message).sessionId,
            childPid(client.initialOutput())};
}
// Collects output until `expected` (acknowledging it); false on error/exit/ I-O
// failure.
bool outputHas(Client &client, std::string_view expected) {
    std::string output;
    try {
        for (unsigned i = 0; i < 60; ++i) {
            const auto message = client.read();
            if (const auto *bytes = std::get_if<protocol::Output>(&message)) {
                output += bytes->data;
                client.send(protocol::Ack{bytes->offset + bytes->data.size()});
                if (output.find(expected) != std::string::npos)
                    return true;
            }
            else if (std::holds_alternative<protocol::Error>(message) ||
                     std::holds_alternative<protocol::Exit>(message))
                return false;
        }
    }
    catch (const std::exception &) {
    }
    return false;
}
} // namespace
TEST_SUIT_BEGIN(SessionsReviewNet)
TEST_CASE("Input from a socket of a rotated login is never delivered, "
          "however fast it arrives") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto oldCookie = fixture.login();
    Client stale;
    stale.connect(fixture, oldCookie);
    const auto [id, pid] = startSession(stale);
    EXPECT(pid > 0);
    const auto cookie = fixture.login(); // Rotates: oldCookie is dead now.
    for (unsigned i = 0; i < 20; ++i) {
        try {
            stale.send(protocol::Input{"echo STALEINPUT$((6*7))\n"});
        }
        catch (const std::exception &) {
            break;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Client fresh;
    fresh.connect(fixture, cookie);
    fresh.send(protocol::Hello{1, 80, 24, 0, id, protocol::featureReplay});
    EXPECT(std::holds_alternative<protocol::Welcome>(fresh.read()));
    fresh.send(protocol::Input{"echo MARK$((1+1))\n"});
    std::string all;
    for (unsigned i = 0; i < 60 && all.find("MARK2") == std::string::npos;
         ++i) {
        const auto message = fresh.read();
        if (const auto *bytes = std::get_if<protocol::Output>(&message)) {
            all += bytes->data;
            fresh.send(protocol::Ack{bytes->offset + bytes->data.size()});
        }
    }
    EXPECT(all.find("MARK2") != std::string::npos);
    EXPECT_EQ(all.find("STALEINPUT42"), std::string::npos);
}
TEST_CASE("An expired cookie closes the socket but the detached shell waits "
          "for the next login") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        [](server::ServerConfig &config) {
            persistent(config);
            config.auth.idleTimeout = std::chrono::seconds(1);
        });
    const auto cookie = fixture.login();
    Client first;
    first.connect(fixture, cookie);
    const auto [id, pid] = startSession(first);
    EXPECT(pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    EXPECT_FALSE(outputHas(first, "never-printed")); // Closed (or an error).
    Client refused;
    bool rejected = false;
    try {
        refused.connect(fixture, cookie);
    }
    catch (const boost::system::system_error &) {
        rejected = true;
    }
    EXPECT(rejected);
    const auto next = fixture.login();
    Client second;
    second.connect(fixture, next);
    second.send(protocol::Hello{1, 80, 24, 0, id, protocol::featureReplay});
    EXPECT(std::holds_alternative<protocol::Welcome>(second.read()));
    EXPECT_EQ(childPid(second.initialOutput()), pid);
}
TEST_CASE("Garbage Hellos from a second socket neither evict nor slow the "
          "attached session") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        persistent);
    const auto cookie = fixture.login();
    Client owner;
    owner.connect(fixture, cookie);
    const auto [id, pid] = startSession(owner);
    EXPECT(pid > 0);
    std::mt19937_64 random(5);
    for (unsigned i = 0; i < 25; ++i) {
        Client probe;
        probe.connect(fixture, cookie);
        std::string badId = id;
        badId[random() % 32] = "ghijklmnop"[random() % 10];
        try {
            probe.send(protocol::Hello{1,
                                       80,
                                       24,
                                       random() % 3 ? 0 : ~std::uint64_t(0),
                                       badId,
                                       protocol::featureReplay});
            const auto reply = probe.read();
            const auto *error = std::get_if<protocol::Error>(&reply);
            EXPECT(error != nullptr);
            if (error)
                EXPECT_EQ(error->reason, std::string("session unavailable"));
        }
        catch (const std::exception &) {
        }
    }
    owner.send(protocol::Input{"echo STILLHERE$((2+3))\n"});
    EXPECT(outputHas(owner, "STILLHERE5"));
}
TEST_SUIT_END
