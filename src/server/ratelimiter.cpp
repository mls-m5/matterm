#include "ratelimiter.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <tuple>

namespace server {
namespace {
namespace ip = boost::asio::ip;
unsigned retry(Clock::duration duration) {
    return static_cast<unsigned>(
        std::chrono::ceil<std::chrono::seconds>(duration).count());
}
constexpr std::chrono::seconds pruneInterval{1};
} // namespace
std::string sourceKey(const ip::address &address) {
    if (address.is_v6()) {
        const auto v6 = address.to_v6();
        if (v6.is_v4_mapped())
            return ip::make_address_v4(ip::v4_mapped, v6).to_string();
        auto bytes = v6.to_bytes();
        std::fill(bytes.begin() + 8, bytes.end(), 0);
        return ip::address_v6(bytes).to_string() + "/64";
    }
    return address.to_v4().to_string();
}
bool peerIsLoopback(const ip::address &address) {
    if (address.is_v6() && address.to_v6().is_v4_mapped())
        return ip::make_address_v4(ip::v4_mapped, address.to_v6())
            .is_loopback();
    return address.is_loopback();
}
RateLimiter::RateLimiter(RateLimitConfig config, Now now)
    : config_(config)
    , now_(std::move(now))
    , window_(now_())
    , nextPrune_(window_) {
    if (!config_.maxFailures || !config_.maxSources ||
        config_.lockoutInitial.count() <= 0 ||
        config_.lockoutMax < config_.lockoutInitial ||
        config_.sourceIdleExpiry.count() <= 0) {
        throw std::invalid_argument("Invalid rate limit configuration");
    }
}
void RateLimiter::prune(Clock::time_point now) {
    nextPrune_ = now + pruneInterval;
    for (auto it = sources_.begin(); it != sources_.end();) {
        const auto &source = it->second;
        if (!locked(source, now) &&
            now - source.lastSeen >= config_.sourceIdleExpiry)
            it = sources_.erase(it);
        else
            ++it;
    }
}
std::optional<unsigned> RateLimiter::makeRoom(Clock::time_point now) {
    // Never evict lockouts to admit an attacker's new addresses; an unlocked
    // source only carries a partial failure count, which is cheap to lose.
    auto victim = sources_.end();
    Clock::time_point soonest = Clock::time_point::max();
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        const auto &source = it->second;
        if (locked(source, now)) {
            soonest = std::min(soonest, source.until);
            continue;
        }
        if (victim == sources_.end() ||
            std::tie(source.failures, source.lastSeen) <
                std::tie(victim->second.failures, victim->second.lastSeen))
            victim = it;
    }
    if (victim == sources_.end())
        return std::max(1u, retry(soonest - now));
    sources_.erase(victim);
    return std::nullopt;
}
unsigned RateLimiter::attempt(const std::string &source, bool loopback) {
    assert(affinity_.check());
    const auto now = now_();
    if (now - window_ >= std::chrono::hours(1)) {
        window_ = now;
        attempts_ = 0;
    }
    if (now >= nextPrune_)
        prune(now);
    auto found = sources_.find(source);
    if (found != sources_.end() && locked(found->second, now)) {
        return retry(found->second.until - now);
    }
    const bool global =
        config_.globalAttemptsPerHour && !(loopback && config_.exemptLoopback);
    if (global && attempts_ >= config_.globalAttemptsPerHour) {
        return retry(window_ + std::chrono::hours(1) - now);
    }
    if (found == sources_.end()) {
        if (sources_.size() >= config_.maxSources) {
            if (const auto refused = makeRoom(now))
                return *refused;
        }
        found = sources_.emplace(source, Source{}).first;
    }
    found->second.lastSeen = now;
    if (global) {
        ++attempts_;
        if (config_.globalCountsFailuresOnly)
            ++found->second.reserved;
    }
    return 0;
}
void RateLimiter::failure(const std::string &source) {
    assert(affinity_.check());
    const auto now = now_();
    auto found = sources_.find(source);
    if (found == sources_.end()) {
        // Evicted while in flight: remember the failure if there is room.
        if (sources_.size() >= config_.maxSources && makeRoom(now))
            return;
        found = sources_.emplace(source, Source{}).first;
    }
    auto &state = found->second;
    state.lastSeen = now;
    // The reserved slot stays spent: this attempt failed.
    if (state.reserved)
        --state.reserved;
    if (state.failures < config_.maxFailures)
        ++state.failures;
    if (state.failures >= config_.maxFailures) {
        state.delay = state.delay.count() == 0
                          ? config_.lockoutInitial
                          : std::min(config_.lockoutMax,
                                     state.delay + std::min(state.delay,
                                                            config_.lockoutMax -
                                                                state.delay));
        state.until = now + state.delay;
    }
}
void RateLimiter::success(const std::string &source) {
    assert(affinity_.check());
    const auto found = sources_.find(source);
    if (found == sources_.end())
        return;
    if (found->second.reserved) {
        if (attempts_)
            --attempts_;
    }
    sources_.erase(found);
}
void RateLimiter::reset() {
    assert(affinity_.check());
    sources_.clear();
    attempts_ = 0;
    window_ = nextPrune_ = now_();
}
} // namespace server
