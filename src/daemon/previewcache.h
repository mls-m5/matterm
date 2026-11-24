#pragma once

// Daemon-only, request-time decoded output fragments. No output-path hooks,
// TerminalModel per entry, raw history copy, or secondary VT parser.
#include "protocoltime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mtd {
class PreviewCache {
public:
    using Read =
        std::function<std::optional<std::string>(std::uint64_t, std::size_t)>;
    static constexpr std::size_t kMaxEntries = 16;
    static constexpr std::size_t kReadBytes = 16u << 10;
    static constexpr std::size_t kBytesPerSecond = 64u << 10;
    static constexpr protocol::Time kExpiry{5000};
    PreviewCache();
    ~PreviewCache();
    PreviewCache(const PreviewCache &) = delete;
    PreviewCache &operator=(const PreviewCache &) = delete;

    // Snapshot oldest/end from the retained ring. Read is synchronous and not
    // retained. null = unavailable/disabled/cold/unprocessed; present empty is
    // known empty. Never return a cached old fragment for an unprocessed end.
    std::optional<std::string> get(const std::string &id,
                                   std::uint64_t oldest,
                                   std::uint64_t end,
                                   const Read &read,
                                   protocol::Time now,
                                   bool includePreview);
    void erase(const std::string &id);
    void clear();
    // Caller schedules actual expiry even when no further requests arrive.
    void expire(protocol::Time now);
    std::optional<protocol::Time> nextExpiry() const;
    std::size_t size() const;
    std::uint64_t decodedBytes()
        const; // Fixed-size diagnostic counter, no contents.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mtd
