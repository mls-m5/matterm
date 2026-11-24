#pragma once
#include "protocoltime.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace server {
class ReplayRing {
public:
    explicit ReplayRing(std::size_t budget = 0)
        : budget_(budget) {}
    ~ReplayRing();
    ReplayRing(const ReplayRing &) = delete;
    ReplayRing &operator=(const ReplayRing &) = delete;
    void append(protocol::Time timestamp,
                std::string_view bytes,
                std::uint64_t serial);
    void dropOldest();
    void clear();
    void disable() {
        clear();
        budget_ = 0;
    }
    std::size_t cost() const {
        return cost_;
    }
    std::uint64_t oldest() const;
    std::uint64_t end() const {
        return end_;
    }
    std::uint64_t safeStart() const;
    protocol::Time oldestTime() const;
    std::optional<protocol::Time> newestTime() const {
        return chunks_.empty()
                   ? std::nullopt
                   : std::optional<protocol::Time>(chunks_.back().timestamp);
    }
    std::uint64_t oldestSerial() const;
    std::optional<std::string> read(std::uint64_t offset,
                                    std::size_t limit) const;

private:
    struct Chunk {
        protocol::Time timestamp;
        std::uint64_t offset, serial;
        std::string bytes;
        std::size_t cost() const {
            return sizeof(Chunk) + bytes.capacity() + 1 + 4 * sizeof(void *);
        }
    };
    std::deque<Chunk> chunks_;
    std::size_t budget_, cost_ = 0;
    std::uint64_t end_ = 0;
};
} // namespace server
