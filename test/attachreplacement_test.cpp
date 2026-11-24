#include "attachreplacement.h"
#include "mls-unit-test/unittest.h"
using namespace frontend;
TEST_SUIT_BEGIN(AttachReplacement)
TEST_CASE("Only a running live matching attachment may replace the unchanged "
          "original") {
    AttachReplacementState state;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Wait);
    state.running = true;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Commit);
    state.originalSame = false;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Abort);
    state.originalSame = true;
    state.originalLive = false;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Abort);
    state.originalLive = true;
    state.targetLive = false;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Abort);
    state.targetLive = true;
    state.targetMatches = false;
    EXPECT(replacementDecision(state, 10, 100) == ReplacementDecision::Abort);
}
TEST_CASE("Failure, takeover, cancellation and deadline never authorize "
          "destroying the original") {
    AttachReplacementState state;
    EXPECT(replacementDecision(state, 99, 100) == ReplacementDecision::Wait);
    EXPECT(replacementDecision(state, 100, 100) == ReplacementDecision::Abort);
    state.failed = true;
    EXPECT(replacementDecision(state, 1, 100) == ReplacementDecision::Abort);
    state.failed = false;
    state.cancelled = true;
    EXPECT(replacementDecision(state, 1, 100) == ReplacementDecision::Abort);
}
TEST_SUIT_END
