#pragma once
#include "ratelimiter.h"
#include <optional>
#include <string>
#include <string_view>

namespace server {
// Initializes libsodium once, throwing on failure.
void initializeCrypto();
std::string hashPassword(std::string_view password);
void savePassword(const std::string &path, std::string_view password);
std::string loadPasswordHash(const std::string &path);
class PasswordVerifier {
public:
    explicit PasswordVerifier(std::string hash = {});
    bool verify(std::string_view password) const;

private:
    std::string hash_;
    bool configured_;
};
struct AuthConfig {
    std::chrono::seconds idleTimeout{43200};
    std::chrono::seconds absoluteTimeout{604800};
    unsigned maxUpgradesPerLogin = 16;
};
// One owner login, rotated on every successful login; only a digest is
// retained.
//
// Threading: not thread safe. Use it only from the server's single io_context
// thread (debug builds assert this; the first calling thread becomes the
// owner). generation() is a plain read and follows the same rule.
class SessionStore {
public:
    explicit SessionStore(AuthConfig config = {},
                          RateLimiter::Now now = Clock::now);
    std::string issue();
    bool valid(std::string_view token, bool touch = true);
    bool claim(std::string_view token);
    // Internal lease admission after a previously authenticated upgrade's
    // Hello. A generation-matched takeover may transiently transfer one slot.
    bool claimGeneration(std::uint64_t generation, bool transfer = false);
    void release(std::uint64_t generation);
    void revoke();
    std::uint64_t generation() const {
        return generation_;
    }
    bool attachedValid(std::uint64_t generation, bool touch);

private:
    AuthConfig config_;
    RateLimiter::Now now_;
    std::string digest_;
    Clock::time_point created_{}, touched_{};
    unsigned claimed_ = 0;
    std::uint64_t generation_ = 0;
    ThreadAffinity affinity_;
};
// Strict, bounded single-field JSON object. Supports JSON escapes including
// Unicode surrogate pairs; rejects extra/duplicate fields and control bytes.
std::optional<std::string> parsePassword(std::string_view json);
std::optional<std::string> sessionCookie(std::string_view cookies);
} // namespace server
