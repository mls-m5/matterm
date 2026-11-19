#include "mls-unit-test/unittest.h"
#include "ptylink.h"

#include <chrono>
#include <thread>

namespace {
struct Collected {
    std::string output;
    int exits = 0;
    int endOfOutput = 0;
    int exitCode = -2;
    int exitSignal = -2;
};

// Pumps until `done` or a 5 s deadline.
template <typename Done>
bool pumpUntil(PtyLink &link, Done done, std::size_t budget = 65536) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        link.pump(budget, budget);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

void attach(PtyLink &link, Collected &collected) {
    link.setEvents({[&](std::string_view bytes) { collected.output += bytes; },
                    [&](int code, int signal) {
                        ++collected.exits;
                        collected.exitCode = code;
                        collected.exitSignal = signal;
                    },
                    {}});
    link.setEndOfOutputHandler([&] { ++collected.endOfOutput; });
}
} // namespace

TEST_SUIT_BEGIN(PtyLink)

TEST_CASE("Output, one end of output, and one exit are reported in order") {
    PtyLink link({PTY_TEST_CHILD, "tty"}, 80, 24);
    Collected collected;
    attach(link, collected);
    EXPECT(pumpUntil(
        link, [&] { return link.finished(); }, 7));
    EXPECT(collected.output.find("TTY:80:24:xterm-256color") !=
           std::string::npos);
    EXPECT_EQ(collected.exits, 1);
    EXPECT_EQ(collected.endOfOutput, 1);
    EXPECT_EQ(collected.exitCode, 7);
    EXPECT_EQ(collected.exitSignal, 0);
    link.pump();
    link.pump();
    EXPECT_EQ(collected.exits, 1); // Never repeated.
    EXPECT_FALSE(link.sendInput("late"));
}

TEST_CASE("Queued input reaches the child, which answers") {
    PtyLink link({PTY_TEST_CHILD, "echo"}, 80, 24);
    Collected collected;
    attach(link, collected);
    EXPECT(pumpUntil(link, [&] {
        return collected.output.find("READY") != std::string::npos;
    }));
    EXPECT(link.sendInput("hello\n"));
    EXPECT(pumpUntil(link, [&] { return link.finished(); }));
    EXPECT(collected.output.find("READ:hello") != std::string::npos);
    EXPECT_EQ(collected.exitCode, 0);
}

TEST_CASE("The input queue is bounded") {
    PtyLink link({PTY_TEST_CHILD, "resize"}, 80, 24);
    Collected collected;
    attach(link, collected);
    EXPECT(pumpUntil(link, [&] {
        return collected.output.find("READY") != std::string::npos;
    }));
    EXPECT_FALSE(link.sendInput(std::string(1024 * 1024 + 1, 'x')));
    EXPECT(link.sendInput(std::string(1024, 'x')));
    EXPECT_FALSE(link.sendInput(std::string(1024 * 1024, 'x')));
}

TEST_CASE("Resize reaches the child and close ends it") {
    PtyLink link({PTY_TEST_CHILD, "resize"}, 80, 24);
    Collected collected;
    attach(link, collected);
    EXPECT(pumpUntil(link, [&] {
        return collected.output.find("READY") != std::string::npos;
    }));
    link.resize(100, 30, 800, 600); // Pixel size is accepted and ignored.
    EXPECT(pumpUntil(link, [&] { return link.finished(); }));
    EXPECT_EQ(collected.exits, 1);
    EXPECT_EQ(collected.exitCode, 0);

    PtyLink holder({PTY_TEST_CHILD, "hold"}, 80, 24);
    Collected held;
    attach(holder, held);
    EXPECT(pumpUntil(holder, [&] { return !held.output.empty(); }));
    holder.close();
    EXPECT(pumpUntil(holder, [&] { return holder.finished(); }));
    EXPECT_EQ(held.exits, 1);
}

TEST_SUIT_END
