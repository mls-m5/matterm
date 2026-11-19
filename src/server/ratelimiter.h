#pragma once

#include "threadaffinity.h"
#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace server {
using Clock = std::chrono::steady_clock;
struct RateLimitConfig {
    unsigned maxFailures = 5;
    std::chrono::seconds lockoutInitial{30};
    std::chrono::seconds lockoutMax{900};
    // Password attempts per hour across all sources; 0 disables the budget.
    unsigned globalAttemptsPerHour = 30;
    bool exemptLoopback = true;
    std::size_t maxSources = 4096;
    // A source with no active lockout and no activity for this long is
    // forgotten (its failure count and escalated delay start over).
    std::chrono::seconds sourceIdleExpiry{3600};
    // true: only failed attempts spend the global budget (a successful login
    // gives its reservation back). false: every attempt spends it. See
    // docs/SERVER.md for the reasoning and the residual risk.
    bool globalCountsFailuresOnly = true;
};
// Limiter key for a peer address: IPv4 as is, IPv4-mapped IPv6 as the IPv4
// address, any other IPv6 address as its /64 prefix ("2001:db8:1:2::/64"; a
// scope id is ignored). Use it for every attempt/failure/success call.
std::string sourceKey(const boost::asio::ip::address &address);
// Loopback check that also recognizes IPv4-mapped loopback (::ffff:127.0.0.1).
bool peerIsLoopback(const boost::asio::ip::address &address);
// Time is injected; callers may serialize/restore state in a future slice.
//
// Threading: not thread safe. Use it only from the server's single io_context
// thread (debug builds assert this; the first calling thread becomes the
// owner).
class RateLimiter {
public:
    using Now = std::function<Clock::time_point()>;
    explicit RateLimiter(RateLimitConfig config = {}, Now now = Clock::now);
    // Atomically checks and reserves one hashing attempt. Zero means admitted;
    // otherwise returns the rounded-up Retry-After in seconds. Call exactly
    // one of failure() or success() for every admitted attempt.
    unsigned attempt(const std::string &source, bool loopback);
    void failure(const std::string &source);
    // Clears the source and, in failures-only mode, returns the attempt's
    // global reservation.
    void success(const std::string &source);
    void reset();
    // Number of sources currently remembered (diagnostics and tests).
    std::size_t trackedSources() const {
        return sources_.size();
    }

private:
    struct Source {
        unsigned failures = 0;
        std::chrono::seconds delay{0};
        Clock::time_point until{};
        Clock::time_point lastSeen{};
        // Global budget slots taken by admitted attempts not yet resolved.
        unsigned reserved = 0;
    };
    bool locked(const Source &source, Clock::time_point now) const {
        return now < source.until;
    }
    // Drops idle, unlocked sources. Called at most once per second.
    void prune(Clock::time_point now);
    // Makes room for one new source by evicting the unlocked source with the
    // fewest failures (least recently seen on ties). Never evicts a lockout;
    // returns the Retry-After to report when every source is locked out.
    std::optional<unsigned> makeRoom(Clock::time_point now);

    RateLimitConfig config_;
    Now now_;
    ThreadAffinity affinity_;
    std::map<std::string, Source> sources_;
    Clock::time_point window_;
    Clock::time_point nextPrune_;
    unsigned attempts_ = 0;
};
} // namespace server
