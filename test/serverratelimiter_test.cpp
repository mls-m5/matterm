#include "auth.h"
#include "mls-unit-test/unittest.h"
#include "ratelimiter.h"
#include "threadaffinity.h"
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace server;
using namespace std::chrono;
namespace {
#ifndef NDEBUG
// Runs `use` once on this thread and once on another, in a child process, and
// reports whether the child died of the debug assertion (SIGABRT).
template <class Use>
bool abortsOnSecondThread(Use use) {
    const pid_t child = ::fork();
    if (child == 0) {
        const int null = ::open("/dev/null", O_WRONLY);
        ::dup2(null, 2); // The assertion message is expected noise.
        use();
        std::thread(use).join();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif
} // namespace
TEST_SUIT_BEGIN(ServerRateLimiter)
TEST_CASE("Backoff doubles after threshold and caps, success resets") {
    Clock::time_point now{};
    RateLimitConfig config;
    config.globalAttemptsPerHour = 0;
    RateLimiter limiter(config, [&] { return now; });
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(limiter.attempt("a", false), 0u);
        limiter.failure("a");
    }
    for (unsigned delay : {30u, 60u, 120u, 240u, 480u, 900u, 900u}) {
        EXPECT_EQ(limiter.attempt("a", false), delay);
        now += milliseconds(1);
        EXPECT_EQ(limiter.attempt("a", false), delay);
        now += seconds(delay) - milliseconds(1);
        EXPECT_EQ(limiter.attempt("a", false), 0u);
        limiter.failure("a");
    }
    EXPECT_EQ(limiter.attempt("b", false), 0u);
    limiter.success("a");
    EXPECT_EQ(limiter.attempt("a", false), 0u);
    limiter.failure("a");
    EXPECT_EQ(limiter.attempt("a", false), 0u);
}
TEST_CASE("Global window and loopback exemption") {
    Clock::time_point now{};
    RateLimitConfig config;
    config.globalAttemptsPerHour = 2;
    RateLimiter limiter(config, [&] { return now; });
    EXPECT_EQ(limiter.attempt("a", false), 0u);
    limiter.failure("a");
    EXPECT_EQ(limiter.attempt("b", false), 0u);
    limiter.failure("b");
    EXPECT_EQ(limiter.attempt("c", false), 3600u);
    EXPECT_EQ(limiter.attempt("local", true), 0u);
    now += seconds(3599);
    EXPECT_EQ(limiter.attempt("c", false), 1u);
    now += seconds(1);
    EXPECT_EQ(limiter.attempt("c", false), 0u);
    config.exemptLoopback = false;
    config.globalAttemptsPerHour = 1;
    RateLimiter strict(config, [&] { return now; });
    EXPECT_EQ(strict.attempt("local", true), 0u);
    strict.failure("local");
    EXPECT_EQ(strict.attempt("local", true), 3600u);
}
TEST_CASE("Source table is bounded and resettable") {
    RateLimitConfig config;
    config.maxSources = 1;
    RateLimiter limiter(config);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(limiter.attempt("a", false), 0u);
        limiter.failure("a");
    }
    // The only slot holds an active lockout, which is never evicted.
    EXPECT(limiter.attempt("b", false) > 0);
    limiter.reset();
    EXPECT_EQ(limiter.attempt("b", false), 0u);
}
TEST_CASE("Idle sources are pruned, active lockouts never") {
    Clock::time_point now{};
    RateLimitConfig config;
    config.globalAttemptsPerHour = 0;
    config.maxSources = 3;
    config.sourceIdleExpiry = seconds(100);
    RateLimiter limiter(config, [&] { return now; });
    // "locked" earns a 30 s lockout; two others merely appear.
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(limiter.attempt("locked", false), 0u);
        limiter.failure("locked");
    }
    EXPECT_EQ(limiter.attempt("x", false), 0u);
    EXPECT_EQ(limiter.attempt("y", false), 0u);
    EXPECT_EQ(limiter.trackedSources(), std::size_t(3));
    now += seconds(10);
    // Table full of fresh entries: a new source still gets in, by evicting an
    // unlocked one, never the lockout.
    EXPECT_EQ(limiter.attempt("z", false), 0u);
    EXPECT_EQ(limiter.trackedSources(), std::size_t(3));
    EXPECT(limiter.attempt("locked", false) > 0);
    // Past the idle expiry the unlocked entries disappear; the lockout stays
    // until it ends (30 s), well before the idle expiry could matter.
    now += seconds(15);
    EXPECT(limiter.attempt("locked", false) > 0);
    now += seconds(200);
    EXPECT_EQ(limiter.attempt("w", false), 0u);
    EXPECT(limiter.trackedSources() <= std::size_t(2));
}
TEST_CASE("A scan from many addresses does not lock out the owner") {
    Clock::time_point now{};
    RateLimitConfig config;
    config.globalAttemptsPerHour = 0;
    config.maxSources = 64;
    RateLimiter limiter(config, [&] { return now; });
    for (unsigned i = 0; i < 1000; ++i) {
        const auto source = "scan" + std::to_string(i);
        EXPECT_EQ(limiter.attempt(source, false), 0u);
        limiter.failure(source);
        now += milliseconds(10);
        EXPECT(limiter.trackedSources() <= std::size_t(64));
    }
    EXPECT_EQ(limiter.attempt("owner", false), 0u);
    limiter.success("owner");
    // Source table full of locked-out sources is the only way to be refused,
    // and then only until the earliest lockout ends.
    RateLimitConfig strict = config;
    strict.maxSources = 2;
    RateLimiter full(strict, [&] { return now; });
    for (const char *source : {"a", "b"}) {
        for (unsigned i = 0; i < 5; ++i) {
            EXPECT_EQ(full.attempt(source, false), 0u);
            full.failure(source);
        }
    }
    EXPECT_EQ(full.attempt("owner", false), 30u);
    now += seconds(30);
    EXPECT_EQ(full.attempt("owner", false), 0u);
}
TEST_CASE("Idle expiry is amortized, not a scan on every call") {
    Clock::time_point now{};
    const auto at = [&](long ms) {
        now = Clock::time_point{} + milliseconds(ms);
    };
    RateLimitConfig config;
    config.globalAttemptsPerHour = 0;
    config.sourceIdleExpiry = seconds(10);
    RateLimiter limiter(config, [&] { return now; });
    at(0);
    EXPECT_EQ(limiter.attempt("A", false), 0u);
    at(500);
    EXPECT_EQ(limiter.attempt("B", false), 0u);
    at(1000);
    EXPECT_EQ(limiter.attempt("C", false), 0u);
    at(10200);
    EXPECT_EQ(limiter.attempt("D", false), 0u); // Scans: A (idle 10.2 s) goes.
    EXPECT_EQ(limiter.trackedSources(), std::size_t(3));
    at(10600);
    EXPECT_EQ(limiter.attempt("E", false), 0u); // No scan yet: B expired, kept.
    EXPECT_EQ(limiter.trackedSources(), std::size_t(4));
    at(11200);
    EXPECT_EQ(limiter.attempt("F", false), 0u); // Next scan: B and C go.
    EXPECT_EQ(limiter.trackedSources(), std::size_t(3));
}
TEST_CASE("Global budget counts failures; success refunds the reservation") {
    Clock::time_point now{};
    RateLimitConfig config;
    config.globalAttemptsPerHour = 3;
    RateLimiter limiter(config, [&] { return now; });
    for (unsigned i = 0; i < 10; ++i) {
        EXPECT_EQ(limiter.attempt("owner", false), 0u);
        limiter.success("owner");
    }
    // Three failures from anywhere spend the budget, and the fourth attempt is
    // refused before hashing.
    for (const char *source : {"a", "b", "c"}) {
        EXPECT_EQ(limiter.attempt(source, false), 0u);
        limiter.failure(source);
    }
    EXPECT_EQ(limiter.attempt("owner", false), 3600u);
    // A success without a reservation never refunds somebody else's slot.
    limiter.success("nobody");
    EXPECT_EQ(limiter.attempt("owner", false), 3600u);
    // Reserved but unresolved attempts count (concurrent hashing is bounded).
    now += seconds(3600);
    EXPECT_EQ(limiter.attempt("p", false), 0u);
    EXPECT_EQ(limiter.attempt("q", false), 0u);
    EXPECT_EQ(limiter.attempt("r", false), 0u);
    EXPECT(limiter.attempt("s", false) > 0);
    // Opt-out: count every attempt, successful ones included.
    config.globalCountsFailuresOnly = false;
    RateLimiter attempts(config, [&] { return now; });
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(attempts.attempt("owner", false), 0u);
        attempts.success("owner");
    }
    EXPECT_EQ(attempts.attempt("owner", false), 3600u);
    // Loopback is exempt and never consumes or refunds budget.
    EXPECT_EQ(limiter.attempt("local", true), 0u);
    limiter.success("local");
    EXPECT(limiter.attempt("s", false) > 0);
    limiter.reset();
    EXPECT_EQ(limiter.attempt("owner", false), 0u);
}
TEST_CASE("Invalid configuration is rejected") {
    RateLimitConfig config;
    config.sourceIdleExpiry = seconds(0);
    bool rejected = false;
    try {
        RateLimiter limiter(config);
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    EXPECT(rejected);
}
TEST_CASE("Source key: IPv4 as is, IPv4-mapped unwrapped, IPv6 by /64") {
    namespace ip = boost::asio::ip;
    EXPECT_EQ(sourceKey(ip::make_address("192.0.2.7")), "192.0.2.7");
    EXPECT_EQ(sourceKey(ip::make_address("::ffff:192.0.2.7")), "192.0.2.7");
    EXPECT_EQ(sourceKey(ip::make_address("2001:db8:1:2:3:4:5:6")),
              "2001:db8:1:2::/64");
    EXPECT_EQ(sourceKey(ip::make_address("2001:db8:1:2:ffff:ffff:ffff:ffff")),
              sourceKey(ip::make_address("2001:db8:1:2::1")));
    EXPECT(sourceKey(ip::make_address("2001:db8:1:3::1")) !=
           sourceKey(ip::make_address("2001:db8:1:2::1")));
    EXPECT_EQ(sourceKey(ip::make_address("fe80::1%1")),
              sourceKey(ip::make_address("fe80::2%7")));
    EXPECT_EQ(sourceKey(ip::make_address("::1")), "::/64");
    // An IPv4 address never collides with an IPv6 prefix.
    EXPECT(sourceKey(ip::make_address("::ffff:0.0.0.0")) != "::/64");
    EXPECT(peerIsLoopback(ip::make_address("127.0.0.1")));
    EXPECT(peerIsLoopback(ip::make_address("::1")));
    EXPECT(peerIsLoopback(ip::make_address("::ffff:127.0.0.1")));
    EXPECT(!peerIsLoopback(ip::make_address("::ffff:192.0.2.1")));
    EXPECT(!peerIsLoopback(ip::make_address("2001:db8::1")));
}
TEST_CASE("ThreadAffinity accepts the first thread and rejects others") {
    ThreadAffinity affinity;
    EXPECT(affinity.check());
    EXPECT(affinity.check());
    bool other = true;
    std::thread([&] { other = affinity.check(); }).join();
    EXPECT(!other);
    EXPECT(affinity.check());
}
#ifndef NDEBUG
TEST_CASE("Debug builds assert single-thread use of the limiter and sessions") {
    EXPECT(abortsOnSecondThread([] {
        static RateLimiter limiter;
        limiter.attempt("a", false);
    }));
    EXPECT(abortsOnSecondThread([] {
        static SessionStore store;
        store.issue();
    }));
    // The same thread any number of times is fine.
    RateLimiter limiter;
    SessionStore store;
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(limiter.attempt("a", false), 0u);
        limiter.failure("a");
        store.issue();
    }
}
#endif
TEST_SUIT_END
