#include "mls-unit-test/unittest.h"
#include "ptysession.h"
#include "terminalmodel.h"

#include <chrono>
#include <thread>

namespace {
void finish(PtySession &session, std::size_t budget = 65536) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!session.finished() && std::chrono::steady_clock::now() < deadline) {
        session.pump(budget, budget);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!session.finished()) {
        throw std::runtime_error("PTY session timed out");
    }
}
} // namespace

TEST_SUIT_BEGIN(PtySession)

TEST_CASE(
    "Real child VT output updates the model and receives parser replies") {
    PtySession session({PTY_TEST_CHILD, "vt"}, 80, 24);
    finish(session, 17); // Deliberately fragment controls and response writes.
    EXPECT_EQ(session.exitStatus()->code, 0);
    EXPECT(session.model().cell(0, 0).codePoint == U'm');
    EXPECT_EQ(session.model().cell(0, 0).foreground.g, 205);
    EXPECT(session.model().cell(0, 1).codePoint == U'R');
    EXPECT(session.model().cell(0, 2).codePoint == U'D');
    EXPECT_FALSE(session.send("after exit"));
}

TEST_CASE("Bounded pumping drains all output even after child exit") {
    PtySession session({PTY_TEST_CHILD, "flood"}, 80, 24);
    finish(session, 4096);
    EXPECT_EQ(session.exitStatus()->code, 0);
    EXPECT_EQ(session.model().historySize(), std::size_t(1000));
    EXPECT(session.model().cell(0, 23).codePoint == U'E');
    EXPECT(session.model().cell(2, 23).codePoint == U'D');
}

TEST_CASE("Outbound queues are bounded and resize reaches the native child") {
    PtySession session({PTY_TEST_CHILD, "resize"}, 80, 24);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.model().cell(0, 0).codePoint != U'R' &&
           std::chrono::steady_clock::now() < deadline) {
        session.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(session.send(std::string(1024 * 1024 + 1, 'x')));
    session.resize(100, 30);
    finish(session);
    EXPECT_EQ(session.exitStatus()->code, 0);
    EXPECT_EQ(session.model().columns(), std::size_t(100));
    EXPECT_EQ(session.model().rows(), std::size_t(30));
}

#ifdef MATTERM_TEST_BASH
TEST_CASE("Interactive bash executes input with a real controlling terminal") {
    PtySession session(
        {MATTERM_TEST_BASH, "--noprofile", "--norc", "-i"}, 80, 24);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.model().cursor().column == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        session.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT(session.send("printf 'MATTERM_INTERACTIVE_OK\\n'; exit\r"));
    finish(session);
    EXPECT_EQ(session.exitStatus()->code, 0);
    bool found = false;
    const std::u32string marker = U"MATTERM_INTERACTIVE_OK";
    for (std::size_t y = 0; y < session.model().rows(); ++y) {
        bool matches = true;
        for (std::size_t x = 0; x < marker.size(); ++x) {
            matches =
                matches && session.model().cell(x, y).codePoint == marker[x];
        }
        found = found || matches;
    }
    EXPECT(found);
}

#endif

TEST_SUIT_END
