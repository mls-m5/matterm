#pragma once

// The work indicator of the tab strip (docs/TABS_PLAN.md section 15): a tab whose panes had
// output in the last few seconds is "working", and a working tab's background alternates once a
// second between its colour and a slightly darker one, in step with every other working tab.
// No SDL, no clock of its own: the owner passes the time, so tests are exact.
#include "tabtypes.h"

#include <cstdint>
#include <map>

namespace frontend {

struct TabPulse {
    static constexpr std::int64_t kWorkingMs = 3000; // Output this recently: working.
    static constexpr std::int64_t kPeriodMs = 1000;  // Light for one period, dark for the next.
    static constexpr int kDarkenPercent = 16;        // How much darker: seen at the edge of the eye, not a flash.

    // The global phase: the same for all tabs.
    static bool dark(std::int64_t nowMs) { return nowMs >= 0 && (nowMs / kPeriodMs) % 2 == 1; }
    // When the phase next changes.
    static std::int64_t nextChange(std::int64_t nowMs) { return (nowMs / kPeriodMs + 1) * kPeriodMs; }
};

// When each pane last produced output, seen by comparing a byte counter on every poll.
class OutputWatch {
public:
    // `bytes` is the pane's total output so far. The first sight of a pane records it as quiet.
    void update(PaneId id, std::uint64_t bytes, std::int64_t nowMs) {
        auto it = seen_.find(id);
        if (it == seen_.end()) {
            seen_.emplace(id, Entry{bytes, -1});
        }
        else if (it->second.bytes != bytes) {
            it->second.bytes = bytes;
            it->second.lastMs = nowMs;
        }
    }
    bool recent(PaneId id, std::int64_t nowMs) const {
        const auto it = seen_.find(id);
        return it != seen_.end() && it->second.lastMs >= 0 && nowMs - it->second.lastMs < TabPulse::kWorkingMs;
    }
    // When `id` stops being recent (for the wake-up), or -1.
    std::int64_t quietAt(PaneId id) const {
        const auto it = seen_.find(id);
        return it == seen_.end() || it->second.lastMs < 0 ? -1 : it->second.lastMs + TabPulse::kWorkingMs;
    }
    void forget(PaneId id) { seen_.erase(id); }
    std::size_t size() const { return seen_.size(); }

private:
    struct Entry {
        std::uint64_t bytes = 0;
        std::int64_t lastMs = -1;
    };
    std::map<PaneId, Entry> seen_;
};

} // namespace frontend
