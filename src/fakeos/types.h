#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// Shared vocabulary of the fakeos core. The core does no I/O: it turns bytes in
// into bytes out, takes time as an argument and randomness from a seed.
namespace fakeos {

using Time = std::chrono::milliseconds;

// What the system wants written to the terminal, and whether it ended.
struct Reaction {
    std::string output;
    std::optional<int> exitCode;
};

// What the terminal in front of the system supports. The animations degrade
// instead of failing.
struct Capabilities {
    bool truecolor = true;
    bool colors256 = true;
    bool alternateScreen = true;
    bool unicode = true;
};

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    friend bool operator==(Rgb a, Rgb b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    friend bool operator!=(Rgb a, Rgb b) {
        return !(a == b);
    }
};

// Small deterministic generator (xorshift64*), so tests and screenshots repeat.
class Rng {
public:
    explicit Rng(std::uint64_t seed = 1)
        : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }
    // Uniform in [0, bound); bound > 0.
    std::uint32_t below(std::uint32_t bound) {
        return std::uint32_t((next() >> 32) % bound);
    }
    // Uniform in [0, 1).
    double unit() {
        return double(next() >> 11) / double(1ull << 53);
    }

private:
    std::uint64_t state_;
};

} // namespace fakeos
