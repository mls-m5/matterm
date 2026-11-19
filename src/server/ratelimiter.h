#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>

namespace server {
using Clock = std::chrono::steady_clock;
struct RateLimitConfig {
    unsigned maxFailures = 5;
    std::chrono::seconds lockoutInitial{30};
    std::chrono::seconds lockoutMax{900};
    unsigned globalAttemptsPerHour = 30;
    bool exemptLoopback = true;
    std::size_t maxSources = 4096;
};
// Time is injected; callers may serialize/restore state in a future slice.
class RateLimiter {
public:
    using Now = std::function<Clock::time_point()>;
    explicit RateLimiter(RateLimitConfig config = {}, Now now = Clock::now);
    // Atomically checks and reserves one hashing attempt. Zero means admitted;
    // otherwise returns the rounded-up Retry-After in seconds.
    unsigned attempt(const std::string &source, bool loopback);
    void failure(const std::string &source);
    void success(const std::string &source);
    void reset();

private:
    struct Source {
        unsigned failures = 0;
        std::chrono::seconds delay{0};
        Clock::time_point until{};
    };
    RateLimitConfig config_;
    Now now_;
    std::map<std::string, Source> sources_;
    Clock::time_point window_;
    unsigned attempts_ = 0;
};
} // namespace server
