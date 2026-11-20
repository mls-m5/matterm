#include "commands.h"
#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>
#include <vector>

// `slow on|off` switches the dramatic typing speed (reveal and pips) while the
// demo runs.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

struct Heard {
    std::vector<Beep> beeps;
};

Config dramatic(Heard *heard = nullptr, int rate = 1250) {
    Config config;
    config.intro = false;
    config.revealCharsPerSecond = rate;
    if (heard) {
        config.sound = true;
        config.onBeep = [heard](const Beep &beep) {
            heard->beeps.push_back(beep);
        };
    }
    return config;
}

void settle(Pc &pc, int frames = 300) {
    for (int i = 0; i < frames && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
}
} // namespace

TEST_SUIT_BEGIN(FakeosSlow)

TEST_CASE("The welcome tells about the dramatic typing speed") {
    Pc pc(dramatic());
    settle(pc);
    EXPECT(pc.shows("No real file systems were harmed."));
    EXPECT(pc.shows("Dramatic typing speed: Enabled (Space skips, 'slow off' "
                    "turns it off)"));
    // The line sits under the first one.
    int harmed = -1, speed = -1;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        if (pc.row(y).find("No real file systems") != std::string::npos)
            harmed = int(y);
        if (pc.row(y).find("Dramatic typing speed") != std::string::npos)
            speed = int(y);
    }
    EXPECT(harmed >= 0 && speed == harmed + 1);
}

TEST_CASE(
    "slow off prints at once and silences the pips; slow on brings them back") {
    Heard heard;
    Pc pc(dramatic(&heard));
    settle(pc);
    pc.run("slow off");
    settle(pc);
    EXPECT(pc.shows("Dramatic typing speed: disabled"));
    heard.beeps.clear();
    pc.run("seq 1 100");
    EXPECT(!pc.sys.revealing()); // Everything is there at once.
    EXPECT(pc.shows("100"));
    EXPECT(pc.last() == kPrompt);
    pc.advance(500);
    EXPECT(heard.beeps.empty());
    pc.run("slow on");
    settle(pc);
    EXPECT(pc.shows("Dramatic typing speed: enabled"));
    pc.run("clear");
    pc.run("seq 1 100");
    EXPECT(pc.sys.revealing());
    settle(pc);
    EXPECT(!heard.beeps.empty());
    EXPECT(pc.shows("100"));
}

TEST_CASE("The welcome file follows the setting") {
    Pc pc(dramatic());
    settle(pc);
    pc.run("slow off");
    pc.run("clear");
    pc.run("cat /etc/motd");
    EXPECT(pc.shows("Dramatic typing speed: Disabled ('slow on' turns it on)"));
    EXPECT(!pc.shows("turns it off"));
    pc.run("slow on");
    settle(pc);
    pc.run("clear");
    pc.run("cat /etc/motd");
    settle(pc);
    EXPECT(pc.shows("Dramatic typing speed: Enabled (Space skips, 'slow off' "
                    "turns it off)"));
}

TEST_CASE("slow without an argument shows the state; a wrong one explains") {
    Pc pc(dramatic());
    settle(pc);
    pc.run("slow");
    settle(pc);
    EXPECT(pc.shows("Dramatic typing speed: enabled"));
    pc.run("slow maybe");
    settle(pc);
    EXPECT(pc.shows("usage: slow on|off"));
    pc.run("echo $?");
    settle(pc);
    EXPECT(pc.shows("\n2\n"));
}

TEST_CASE(
    "A host without the pace starts with it off, and slow on still works") {
    Pc pc(dramatic(nullptr, 0));
    EXPECT(pc.shows("Dramatic typing speed: Disabled ('slow on' turns it on)"));
    pc.run("seq 1 50");
    EXPECT(!pc.sys.revealing());
    pc.run("slow on");
    settle(pc);
    pc.run("clear");
    pc.run("seq 1 50");
    EXPECT(pc.sys.revealing());
    settle(pc);
    EXPECT(pc.shows("50"));
}

TEST_CASE("reset goes back to what the host started with") {
    Pc pc(dramatic());
    settle(pc);
    pc.run("slow off");
    pc.run("reset");
    settle(pc);
    pc.run("cat /etc/motd");
    settle(pc);
    EXPECT(pc.shows("Dramatic typing speed: Enabled"));
}

TEST_CASE("help mentions slow, and it works without a file (a builtin)") {
    Pc pc(dramatic(nullptr, 0));
    pc.run("help");
    EXPECT(pc.shows("slow"));
    EXPECT(pc.shows("'slow off'") || pc.shows("slow on|off"));
    const auto *command = findCommand("slow");
    EXPECT(command && command->builtin); // Found even if /bin is gone.
}

TEST_SUIT_END
