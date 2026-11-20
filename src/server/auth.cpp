#include "auth.h"
#include "message.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sodium.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace server {
void initializeCrypto() {
    static const int result = sodium_init();
    if (result < 0)
        throw std::runtime_error("libsodium initialization failed");
}
std::string hashPassword(std::string_view password) {
    initializeCrypto();
    if (password.size() < 24 || password.size() > 256)
        throw std::invalid_argument("Password must be 24..256 bytes");
    std::array<char, crypto_pwhash_STRBYTES> hash{};
    if (crypto_pwhash_str_alg(hash.data(),
                              password.data(),
                              password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE,
                              crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw std::runtime_error("Password hashing failed");
    return hash.data();
}
void savePassword(const std::string &path, std::string_view password) {
    const auto hash = hashPassword(password) + "\n";
    // Do not follow symlinks, truncate hardlinks, or leave a partial old hash.
    const auto temporary = path + ".new";
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0)
        throw std::runtime_error("Cannot create credentials temporary file");
    const bool ok = ::write(fd, hash.data(), hash.size()) ==
                        static_cast<ssize_t>(hash.size()) &&
                    ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!ok || !closed || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("Cannot save credentials");
    }
}
std::string loadPasswordHash(const std::string &path) {
    if (path.empty())
        return {};
    const int fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        throw std::runtime_error("Cannot open credentials");
    struct stat st {};
    std::array<char, crypto_pwhash_STRBYTES + 1> data{};
    const bool safe = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                      st.st_uid == ::geteuid() && (st.st_mode & 077) == 0;
    const auto size = safe ? ::read(fd, data.data(), data.size()) : -1;
    ::close(fd);
    if (size <= 0 || size >= static_cast<ssize_t>(data.size()))
        throw std::runtime_error(
            "Invalid credentials file (requires owner-only permissions)");
    std::string result(data.data(), size);
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    if (result.find('\0') != std::string::npos ||
        result.rfind("$argon2id$", 0) != 0 ||
        crypto_pwhash_str_needs_rehash(result.c_str(),
                                       crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                       crypto_pwhash_MEMLIMIT_INTERACTIVE) < 0)
        throw std::runtime_error("Invalid Argon2id credentials");
    return result;
}
PasswordVerifier::PasswordVerifier(std::string hash)
    : hash_(std::move(hash))
    , configured_(!hash_.empty()) {
    initializeCrypto();
    if (!configured_)
        hash_ = hashPassword("dummy password not used for authentication");
}
bool PasswordVerifier::verify(std::string_view password) const {
    const bool matched = crypto_pwhash_str_verify(hash_.c_str(),
                                                  password.data(),
                                                  password.size()) == 0;
    return matched && configured_;
}
namespace {
std::string digest(std::string_view token) {
    std::string result(crypto_generichash_BYTES, '\0');
    crypto_generichash(reinterpret_cast<unsigned char *>(result.data()),
                       result.size(),
                       reinterpret_cast<const unsigned char *>(token.data()),
                       token.size(),
                       nullptr,
                       0);
    return result;
}
} // namespace
SessionStore::SessionStore(AuthConfig config, RateLimiter::Now now)
    : config_(config)
    , now_(std::move(now)) {
    initializeCrypto();
}
std::string SessionStore::issue() {
    assert(affinity_.check());
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 65> hex{};
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    std::string token(hex.data());
    digest_ = digest(token);
    created_ = touched_ = now_();
    claimed_ = 0;
    ++generation_;
    return token;
}
bool SessionStore::valid(std::string_view token, bool touch) {
    assert(affinity_.check());
    if (token.size() != 64 || digest_.empty())
        return false;
    const auto candidate = digest(token);
    if (sodium_memcmp(candidate.data(), digest_.data(), digest_.size()) != 0)
        return false;
    const auto now = now_();
    if (now - created_ >= config_.absoluteTimeout ||
        now - touched_ >= config_.idleTimeout) {
        revoke();
        return false;
    }
    if (touch)
        touched_ = now;
    return true;
}
bool SessionStore::claim(std::string_view token) {
    assert(affinity_.check());
    return valid(token) && claimGeneration(generation_);
}
bool SessionStore::claimGeneration(std::uint64_t generation, bool transfer) {
    assert(affinity_.check());
    if (!attachedValid(generation, false) ||
        claimed_ >= config_.maxUpgradesPerLogin + unsigned(transfer))
        return false;
    ++claimed_;
    return true;
}
void SessionStore::release(std::uint64_t generation) {
    assert(affinity_.check());
    if (generation == generation_ && claimed_)
        --claimed_;
}
bool SessionStore::attachedValid(std::uint64_t generation, bool touch) {
    assert(affinity_.check());
    if (generation != generation_ || digest_.empty())
        return false;
    const auto now = now_();
    if (now - created_ >= config_.absoluteTimeout ||
        now - touched_ >= config_.idleTimeout) {
        revoke();
        return false;
    }
    if (touch)
        touched_ = now;
    return true;
}
void SessionStore::revoke() {
    assert(affinity_.check());
    digest_.clear();
    claimed_ = 0;
}
namespace {
class Json {
public:
    explicit Json(std::string_view input)
        : input_(input) {}
    bool take(char c) {
        space();
        if (pos_ == input_.size() || input_[pos_] != c)
            return false;
        ++pos_;
        return true;
    }
    void space() {
        while (pos_ < input_.size() &&
               (input_[pos_] == ' ' || input_[pos_] == '\t' ||
                input_[pos_] == '\r' || input_[pos_] == '\n'))
            ++pos_;
    }
    bool end() {
        space();
        return pos_ == input_.size();
    }
    std::optional<std::string> string() {
        if (!take('"'))
            return {};
        std::string out;
        while (pos_ < input_.size()) {
            unsigned char c = input_[pos_++];
            if (c == '"')
                return out;
            if (c < 32)
                return {};
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ == input_.size())
                return {};
            switch (input_[pos_++]) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                auto value = hex4();
                if (!value)
                    return {};
                unsigned cp = *value;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (pos_ + 2 > input_.size() ||
                        input_.substr(pos_, 2) != "\\u")
                        return {};
                    pos_ += 2;
                    auto low = hex4();
                    if (!low || *low < 0xdc00 || *low > 0xdfff)
                        return {};
                    cp = 0x10000 + ((cp - 0xd800) << 10) + *low - 0xdc00;
                }
                else if (cp >= 0xdc00 && cp <= 0xdfff)
                    return {};
                if (cp < 0x80)
                    out += static_cast<char>(cp);
                else {
                    if (cp >= 0x10000)
                        out += static_cast<char>(0xf0 | (cp >> 18));
                    if (cp >= 0x800)
                        out += static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) |
                                                 ((cp >> 12) & 63));
                    out += static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) |
                                             ((cp >> 6) & 63));
                    out += static_cast<char>(0x80 | (cp & 63));
                }
                break;
            }
            default:
                return {};
            }
        }
        return {};
    }

private:
    std::optional<unsigned> hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == input_.size())
                return {};
            char c = input_[pos_++];
            unsigned v;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else
                return {};
            result = result * 16 + v;
        }
        return result;
    }
    std::string_view input_;
    std::size_t pos_ = 0;
};
} // namespace
std::optional<std::string> parsePassword(std::string_view json) {
    if (json.size() > 512 || !protocol::validUtf8(json))
        return {};
    Json parser(json);
    if (!parser.take('{'))
        return {};
    auto key = parser.string();
    if (!key || *key != "password" || !parser.take(':'))
        return {};
    auto password = parser.string();
    if (!password || password->size() > 256 || !parser.take('}') ||
        !parser.end())
        return {};
    return password;
}
std::optional<std::string> sessionCookie(std::string_view cookies) {
    std::optional<std::string> token;
    while (!cookies.empty()) {
        const auto end = cookies.find(';');
        auto item = cookies.substr(0, end);
        while (!item.empty() && item.front() == ' ')
            item.remove_prefix(1);
        if (item.substr(0, 16) == "matterm_session=") {
            if (token)
                return {};
            token = std::string(item.substr(16));
        }
        if (end == std::string_view::npos)
            break;
        cookies.remove_prefix(end + 1);
    }
    return token;
}
} // namespace server
