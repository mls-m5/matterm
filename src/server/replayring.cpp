#include "replayring.h"
#include <algorithm>
#include <limits>
#include <sodium.h>
#include <stdexcept>

namespace server {
ReplayRing::~ReplayRing() {
    clear();
}
void ReplayRing::dropOldest() {
    if (chunks_.empty())
        return;
    auto &chunk = chunks_.front();
    cost_ -= chunk.cost();
    sodium_memzero(chunk.bytes.data(), chunk.bytes.size());
    chunks_.pop_front();
}
void ReplayRing::clear() {
    while (!chunks_.empty())
        dropOldest();
}
void ReplayRing::append(protocol::Time timestamp,
                        std::string_view bytes,
                        std::uint64_t serial) {
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - end_)
        throw std::overflow_error("terminal output offset exhausted");
    const auto begin = end_;
    end_ += bytes.size();
    if (bytes.empty() || budget_ <= sizeof(Chunk) + 1 + 4 * sizeof(void *))
        return;
    const auto count = std::min(
        bytes.size(), budget_ - sizeof(Chunk) - 1 - 4 * sizeof(void *));
    Chunk chunk{timestamp,
                begin + bytes.size() - count,
                serial,
                std::string(bytes.substr(bytes.size() - count))};
    if (chunk.cost() > budget_) {
        sodium_memzero(chunk.bytes.data(), chunk.bytes.size());
        clear();
        return;
    }
    while (cost_ > budget_ - chunk.cost())
        dropOldest();
    // All allocations finish before committing accounting.
    chunks_.push_back(std::move(chunk));
    cost_ += chunks_.back().cost();
}
std::uint64_t ReplayRing::oldest() const {
    return chunks_.empty() ? end_ : chunks_.front().offset;
}
std::uint64_t ReplayRing::safeStart() const {
    if (oldest() == 0)
        return 0;
    for (const auto &chunk : chunks_) {
        const auto newline = chunk.bytes.find('\n');
        if (newline != std::string::npos)
            return chunk.offset + newline + 1;
    }
    return end_;
}
protocol::Time ReplayRing::oldestTime() const {
    return chunks_.empty() ? protocol::Time::max() : chunks_.front().timestamp;
}
std::uint64_t ReplayRing::oldestSerial() const {
    return chunks_.empty() ? std::numeric_limits<std::uint64_t>::max()
                           : chunks_.front().serial;
}
std::optional<std::string> ReplayRing::read(std::uint64_t offset,
                                            std::size_t limit) const {
    if (offset < oldest() || offset > end_)
        return {};
    std::string result;
    for (const auto &chunk : chunks_) {
        const auto finish = chunk.offset + chunk.bytes.size();
        if (offset >= finish)
            continue;
        if (offset < chunk.offset)
            return {};
        const auto count =
            std::min<std::size_t>(limit - result.size(), finish - offset);
        result.append(chunk.bytes, std::size_t(offset - chunk.offset), count);
        offset += count;
        if (result.size() == limit)
            break;
    }
    return result;
}
} // namespace server
