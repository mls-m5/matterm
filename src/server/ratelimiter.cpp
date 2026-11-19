#include "ratelimiter.h"
#include <algorithm>
#include <stdexcept>

namespace server {
namespace {
unsigned retry(Clock::duration duration) {
    return static_cast<unsigned>(
        std::chrono::ceil<std::chrono::seconds>(duration).count());
}
} // namespace
RateLimiter::RateLimiter(RateLimitConfig config, Now now)
    : config_(config)
    , now_(std::move(now))
    , window_(now_()) {
    if (!config_.maxFailures || !config_.maxSources ||
        config_.lockoutInitial.count() <= 0 ||
        config_.lockoutMax < config_.lockoutInitial) {
        throw std::invalid_argument("Invalid rate limit configuration");
    }
}
unsigned RateLimiter::attempt(const std::string &source, bool loopback) {
    const auto now = now_();
    if (now - window_ >= std::chrono::hours(1)) {
        window_ = now;
        attempts_ = 0;
    }
    auto found = sources_.find(source);
    if (found != sources_.end() && now < found->second.until) {
        return retry(found->second.until - now);
    }
    const bool global =
        config_.globalAttemptsPerHour && !(loopback && config_.exemptLoopback);
    if (global && attempts_ >= config_.globalAttemptsPerHour) {
        return retry(window_ + std::chrono::hours(1) - now);
    }
    // Never evict lockouts to admit an attacker's new addresses.
    if (found == sources_.end()) {
        if (sources_.size() >= config_.maxSources)
            return 3600;
        sources_.emplace(source, Source{});
    }
    if (global)
        ++attempts_;
    return 0;
}
void RateLimiter::failure(const std::string &source) {
    auto found = sources_.find(source);
    if (found == sources_.end())
        return;
    auto &state = found->second;
    if (state.failures < config_.maxFailures)
        ++state.failures;
    if (state.failures >= config_.maxFailures) {
        state.delay = state.delay.count() == 0
                          ? config_.lockoutInitial
                          : std::min(config_.lockoutMax,
                                     state.delay + std::min(state.delay,
                                                            config_.lockoutMax -
                                                                state.delay));
        state.until = now_() + state.delay;
    }
}
void RateLimiter::success(const std::string &source) {
    sources_.erase(source);
}
void RateLimiter::reset() {
    sources_.clear();
    attempts_ = 0;
    window_ = now_();
}
} // namespace server
