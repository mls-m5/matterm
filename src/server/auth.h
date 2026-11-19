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
};
// One owner login, rotated on every successful login; only a digest is
// retained.
class SessionStore {
public:
    explicit SessionStore(AuthConfig config = {},
                          RateLimiter::Now now = Clock::now);
    std::string issue();
    bool valid(std::string_view token, bool touch = true);
    bool claim(std::string_view token);
    void revoke();

private:
    AuthConfig config_;
    RateLimiter::Now now_;
    std::string digest_;
    Clock::time_point created_{}, touched_{};
    bool claimed_ = false;
};
// Strict, bounded single-field JSON object. Supports JSON escapes including
// Unicode surrogate pairs; rejects extra/duplicate fields and control bytes.
std::optional<std::string> parsePassword(std::string_view json);
std::optional<std::string> sessionCookie(std::string_view cookies);
} // namespace server
