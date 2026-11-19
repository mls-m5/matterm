#include "mls-unit-test/unittest.h"
#include "ratelimiter.h"
using namespace server;
using namespace std::chrono;
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
    limiter.success("a");
    EXPECT_EQ(limiter.attempt("b", false), 0u);
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
    EXPECT_EQ(strict.attempt("local", true), 3600u);
}
TEST_CASE("Source table is bounded and resettable") {
    RateLimitConfig config;
    config.maxSources = 1;
    RateLimiter limiter(config);
    EXPECT_EQ(limiter.attempt("a", false), 0u);
    EXPECT(limiter.attempt("b", false) > 0);
    limiter.reset();
    EXPECT_EQ(limiter.attempt("b", false), 0u);
}
TEST_SUIT_END
