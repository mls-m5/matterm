#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <algorithm>
#include <string>
#include <vector>

// The start command (neofetch in the demos) is typed at the first prompt, as a
// visitor would, once the intro and the welcome text are done.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

struct Heard {
    std::vector<Beep> beeps;
};

Config autorun(Heard *heard = nullptr, bool intro = true, int rate = 1250) {
    Config config;
    config.intro = intro;
    config.startCommand = "neofetch";
    config.revealCharsPerSecond = rate;
    if (heard) {
        config.sound = true;
        config.onBeep = [heard](const Beep &beep) {
            heard->beeps.push_back(beep);
        };
    }
    return config;
}

// Time passes in frames, as a host ticks.
void walk(Pc &pc, long long ms) {
    for (long long t = 0; t < ms; t += 33) {
        pc.advance(33);
    }
}

// Skip the intro and let the welcome text finish.
void toTheFirstPrompt(Pc &pc) {
    pc.advance(300);
    pc.type("x");
    pc.advance(1400);
    for (int i = 0; i < 100 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
}
} // namespace

TEST_SUIT_BEGIN(FakeosAutorun)

TEST_CASE("The command is typed letter by letter, then run, then the prompt "
          "returns") {
    Pc pc(autorun());
    toTheFirstPrompt(pc);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    std::vector<std::string> seen;
    long long elapsed = 0;
    while (elapsed < 2500 &&
           !pc.sys.revealing()) { // Typing, a pause, then Enter.
        pc.advance(30);
        elapsed += 30;
        if (pc.last().rfind(kPrompt + " ", 0) == 0) {
            const auto typed = pc.last().substr(kPrompt.size() + 1);
            if (seen.empty() || seen.back() != typed) {
                seen.push_back(typed);
            }
        }
    }
    // The prefix grows one or two letters at a time up to the whole word.
    EXPECT(seen.size() >= 6);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        EXPECT(seen[i].size() >= seen[i - 1].size() &&
               seen[i].size() <= seen[i - 1].size() + 2);
    }
    EXPECT(seen.back() == "neofetch");
    EXPECT(
        pc.sys.revealing()); // Enter was pressed: the output is being revealed.
    for (int i = 0; i < 400 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
    EXPECT(pc.shows("by Mattias Lasersk"));
    EXPECT(pc.last() == kPrompt); // And an empty prompt.
}

TEST_CASE("Typing the command makes pips, one per letter") {
    Heard heard;
    Pc pc(autorun(&heard));
    toTheFirstPrompt(pc);
    heard.beeps.clear();
    long long elapsed = 0;
    while (elapsed < 2500 && !pc.sys.revealing()) {
        pc.advance(20);
        elapsed += 20;
    }
    // Eight letters, then Enter starts the reveal (whose pips come after).
    const auto typing = std::count_if(
        heard.beeps.begin(), heard.beeps.end(), [](const Beep &b) {
            return b.frequency >= 1100 && b.durationMs <= 20;
        });
    EXPECT(typing >= 6 && typing <= 12);
}

TEST_CASE("Space finishes the typing and runs the command; it is not typed") {
    Pc pc(autorun());
    toTheFirstPrompt(pc);
    walk(pc, 700); // Part of the way.
    EXPECT(pc.last().size() > kPrompt.size() + 1);
    EXPECT(pc.last() != kPrompt + " neofetch");
    pc.type(" ");
    for (int i = 0; i < 400 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Another key does the same and is typed ahead") {
    Pc pc(autorun());
    toTheFirstPrompt(pc);
    walk(pc, 600);
    pc.type("q");
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
    EXPECT(pc.last() == kPrompt + " q");
}

TEST_CASE("It runs once, and not at all without a start command") {
    Pc pc(autorun());
    toTheFirstPrompt(pc);
    for (int i = 0; i < 300 && (pc.sys.revealing() || pc.last() != kPrompt);
         ++i) {
        pc.advance(30);
    }
    pc.run("reset");
    for (int i = 0; i < 200; ++i) {
        pc.advance(30);
    }
    pc.run("clear");
    for (int i = 0; i < 100; ++i) {
        pc.advance(30);
    }
    EXPECT(!pc.shows("Kernel:"));
    Config plain;
    plain.intro = true;
    plain.revealCharsPerSecond = 1250;
    Pc empty(plain);
    toTheFirstPrompt(empty);
    for (int i = 0; i < 200; ++i) {
        empty.advance(30);
    }
    EXPECT(!empty.shows("Kernel:"));
    EXPECT(empty.last() == kPrompt);
}

TEST_CASE("Without the pace the command is typed and run at once") {
    Pc pc(autorun(nullptr, false, 0));
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Without the intro the command still waits for the welcome") {
    Pc pc(autorun(nullptr, false, 1250));
    EXPECT(pc.sys.revealing());
    EXPECT(!pc.shows("neofetch"));
    for (int i = 0; i < 500; ++i) {
        pc.advance(20);
    }
    EXPECT(pc.shows("Kernel: 6.8.0-mattrix"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("The same run gives the same stream") {
    auto film = [] {
        Pc pc(autorun());
        toTheFirstPrompt(pc);
        for (int i = 0; i < 250; ++i) {
            pc.advance(30);
        }
        return pc.raw;
    };
    EXPECT(film() == film());
}

TEST_SUIT_END
