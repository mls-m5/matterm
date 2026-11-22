// A real shell in a real PTY: what probePane() reports at the prompt, in another folder and
// while a program runs (docs/TABS_PLAN.md section 9).
#include "mls-unit-test/unittest.h"
#include "procinfo.h"
#include "ptylink.h"

#include <chrono>
#include <thread>
#include <unistd.h>

using namespace frontend;

namespace {
template <class Pred>
bool until(PtyLink &link, Pred pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        link.pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
} // namespace

TEST_SUIT_BEGIN(PaneState)

TEST_CASE("Prompt, another folder, a running program, back at the prompt") {
    PtyLink link({"/bin/bash", "--norc", "--noprofile", "-i"}, 80, 24, "xterm-256color");
    link.setEvents({});
    LinuxProcSource proc;
    const int shell = link.childPid();
    EXPECT(shell > 0);
    // Ready: the shell's own group is the foreground one.
    EXPECT(until(link, [&] { return link.foregroundProcessGroup() == shell; }));
    auto state = probePane(proc, shell, link.foregroundProcessGroup());
    EXPECT(!state.busy && state.command.empty());
    EXPECT(!state.cwd.empty());
    // cd somewhere else; the folder follows without any shell setup.
    EXPECT(link.sendInput("cd /usr\n"));
    EXPECT(until(link, [&] {
        const auto s = probePane(proc, shell, link.foregroundProcessGroup());
        return s.cwd == "/usr";
    }));
    // A program in the foreground: busy, with its command line and its folder.
    EXPECT(link.sendInput("cd /tmp && sleep 30\n"));
    EXPECT(until(link, [&] { return link.foregroundProcessGroup() != shell && link.foregroundProcessGroup() != 0; }));
    state = probePane(proc, shell, link.foregroundProcessGroup());
    EXPECT(state.busy);
    EXPECT_EQ(state.command.size(), std::size_t(2));
    EXPECT_EQ(state.command[0], std::string("sleep"));
    EXPECT_EQ(state.command[1], std::string("30"));
    EXPECT_EQ(state.cwd, std::string("/tmp"));
    EXPECT_EQ(commandLine(state.command), std::string("sleep 30"));
    // Interrupt it: the prompt is back.
    EXPECT(link.sendInput("\x03"));
    EXPECT(until(link, [&] { return link.foregroundProcessGroup() == shell; }));
    EXPECT(!probePane(proc, shell, link.foregroundProcessGroup()).busy);
    link.close();
}

TEST_SUIT_END
