#pragma once
#include <cstdint>

namespace frontend {
enum class ReplacementDecision { Wait, Commit, Abort };
struct AttachReplacementState {
    bool originalSame = true, originalLive = true;
    bool targetLive = true, targetMatches = true;
    bool running = false, failed = false, cancelled = false;
};
inline ReplacementDecision replacementDecision(
    const AttachReplacementState &state,
    std::int64_t now,
    std::int64_t deadline) {
    if (!state.originalSame || !state.originalLive || !state.targetLive ||
        !state.targetMatches || state.failed || state.cancelled ||
        now >= deadline)
        return ReplacementDecision::Abort;
    return state.running ? ReplacementDecision::Commit
                         : ReplacementDecision::Wait;
}
} // namespace frontend
