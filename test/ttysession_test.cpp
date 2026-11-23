#include "mls-unit-test/unittest.h"
#include "ptylink.h"

#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <unistd.h>

// tty::Session under a real pseudo-terminal: the probe program enters the
// alternate screen, follows resizes, and the terminal is put back on every way
// out.
namespace {
struct Probe {
    Probe()
        : link({TTY_PROBE},
               80,
               24,
               "xterm-256color",
               PtyProcess::ColorCapability::TrueColor) {
        link.setEvents(
            {[this](std::string_view bytes) { output.append(bytes); },
             [this](int code, int signal) {
                 exited = true;
                 exitCode = code;
                 exitSignal = signal;
             },
             {}});
    }
    bool until(const std::string &needle, int ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (output.find(needle, from) == std::string::npos &&
               std::chrono::steady_clock::now() < deadline && !exited) {
            link.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto at = output.find(needle, from);
        if (at != std::string::npos) {
            from = at + needle.size();
        }
        return at != std::string::npos;
    }
    bool untilExit(int ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!exited && std::chrono::steady_clock::now() < deadline) {
            link.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return exited;
    }
    PtyLink link;
    std::string output;
    std::size_t from = 0;
    bool exited = false;
    int exitCode = -2;
    int exitSignal = 0;
};
} // namespace

TEST_SUIT_BEGIN(TtySession)

TEST_CASE("Starts on the alternate screen, draws and restores on q") {
    Probe p;
    EXPECT(p.until("\x1b[?1049h"));
    EXPECT(p.until("size 80x24"));
    p.link.sendInput("q");
    EXPECT(p.until("\x1b[?1049l"));
    EXPECT(p.untilExit());
    EXPECT_EQ(p.exitCode, 0);
    EXPECT(p.output.rfind("\x1b[?25h") > p.output.find("\x1b[?25l"));
}

TEST_CASE("A window resize redraws at the new size") {
    Probe p;
    EXPECT(p.until("size 80x24"));
    p.link.resize(100, 30);
    EXPECT(p.until("size 100x30"));
    p.link.sendInput("q");
    EXPECT(p.untilExit());
}

TEST_CASE("SIGTERM leaves the terminal restored and the program decides") {
    Probe p;
    EXPECT(p.until("size 80x24"));
    kill(p.link.childPid(), SIGTERM);
    EXPECT(p.until("\x1b[?1049l"));
    EXPECT(p.untilExit());
    EXPECT_EQ(p.exitCode, 3);
}

TEST_CASE("A crash restores the terminal before the process dies") {
    Probe p;
    EXPECT(p.until("size 80x24"));
    p.link.sendInput("c"); // SIGSEGV.
    EXPECT(p.until("\x1b[?1049l"));
    EXPECT(p.untilExit());
    EXPECT_EQ(p.exitSignal, SIGSEGV);
}

TEST_CASE("abort() restores the terminal too") {
    Probe p;
    EXPECT(p.until("size 80x24"));
    p.link.sendInput("a");
    EXPECT(p.until("\x1b[?1049l"));
    EXPECT(p.untilExit());
    EXPECT_EQ(p.exitSignal, SIGABRT);
}

TEST_CASE(
    "Stop gives the terminal back, continue takes it again and repaints") {
    Probe p;
    EXPECT(p.until("size 80x24"));
    kill(p.link.childPid(), SIGTSTP);
    EXPECT(p.until("\x1b[?1049l"));
    kill(p.link.childPid(), SIGCONT);
    EXPECT(p.until("\x1b[?1049h"));
    EXPECT(p.until("size 80x24")); // Repainted from scratch.
    p.link.sendInput("q");
    EXPECT(p.untilExit());
    EXPECT_EQ(p.exitCode, 0);
}

TEST_SUIT_END
