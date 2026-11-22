#pragma once

// The work indicator of the tab strip (docs/TABS_PLAN.md section 15): a tab whose panes had
// output in the last few seconds is "working", and a working tab's background alternates once a
// second between its colour and a slightly darker one, in step with every other working tab.
// No SDL, no clock of its own: the owner passes the time, so tests are exact.
#include "tabtypes.h"

#include <algorithm>
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

// When each pane last produced output that counts, seen by comparing byte counters on every poll.
// Output that follows the user's own input (the echo, a program reacting to a key) or the start of
// the pane does not count: it is not the program working on its own.
class OutputWatch {
public:
    struct Settings {
        std::int64_t ignoreAfterInputMs = 5000; // Output this soon after input to the pane is ignored.
        std::int64_t ignoreAfterStartMs = 5000; // And this soon after the pane was first seen.
    };

    // `outputBytes`/`inputBytes`: totals so far for the pane. The first sight of a pane records it
    // as quiet and starts the start grace.
    void update(PaneId id, std::uint64_t outputBytes, std::uint64_t inputBytes, std::int64_t nowMs,
                const Settings &settings) {
        auto it = seen_.find(id);
        if (it == seen_.end()) {
            Entry e;
            e.output = outputBytes;
            e.input = inputBytes;
            e.ignoreUntilMs = nowMs + settings.ignoreAfterStartMs;
            seen_.emplace(id, e);
            return;
        }
        Entry &e = it->second;
        if (e.input != inputBytes) {
            e.input = inputBytes;
            e.ignoreUntilMs = std::max(e.ignoreUntilMs, nowMs + settings.ignoreAfterInputMs);
        }
        if (e.output != outputBytes) {
            e.output = outputBytes;
            if (nowMs >= e.ignoreUntilMs) e.lastMs = nowMs;
        }
    }
    void update(PaneId id, std::uint64_t outputBytes, std::uint64_t inputBytes, std::int64_t nowMs) {
        update(id, outputBytes, inputBytes, nowMs, Settings{});
    }
    bool recent(PaneId id, std::int64_t nowMs) const {
        const auto it = seen_.find(id);
        return it != seen_.end() && it->second.lastMs >= 0 && nowMs - it->second.lastMs < TabPulse::kWorkingMs;
    }
    void forget(PaneId id) { seen_.erase(id); }
    std::size_t size() const { return seen_.size(); }

private:
    struct Entry {
        std::uint64_t output = 0, input = 0;
        std::int64_t lastMs = -1;
        std::int64_t ignoreUntilMs = 0;
    };
    std::map<PaneId, Entry> seen_;
};

} // namespace frontend
