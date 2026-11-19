#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <algorithm>
#include <string>
#include <vector>

// The core only reports pips (Config::onBeep); a host plays them. These tests
// listen to the events.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
struct Heard {
    std::vector<Beep> beeps;
};

Config listening(Heard &heard, bool sound = true, int rate = 1250) {
    Config config;
    config.intro = false;
    config.revealCharsPerSecond = rate;
    config.sound = sound;
    config.onBeep = [&heard](const Beep &beep) { heard.beeps.push_back(beep); };
    return config;
}

void settle(Pc &pc, long long ms = 6000) {
    for (long long t = 0; t < ms && pc.sys.revealing(); t += 20) {
        pc.advance(20);
    }
}
} // namespace

TEST_SUIT_BEGIN(FakeosSound)

TEST_CASE("Nothing is reported unless sound is switched on") {
    Heard heard;
    Pc pc(listening(heard, false));
    pc.run("seq 1 100");
    settle(pc);
    EXPECT(heard.beeps.empty());
    Heard none;
    Config config = listening(none);
    config.onBeep = nullptr; // Sound on, nobody listening: still fine.
    Pc quiet(config);
    quiet.run("seq 1 50");
    settle(quiet);
    EXPECT(quiet.shows("50"));
}

TEST_CASE(
    "Revealed text pips: a pip per few characters, a lower one per line") {
    Heard heard;
    Pc pc(listening(heard));
    pc.run("seq 1 200");
    settle(pc);
    EXPECT(!pc.sys.revealing());
    EXPECT(heard.beeps.size() >= 20);
    EXPECT(heard.beeps.size() <= 120);
    bool low = false, high = false;
    for (const auto &b : heard.beeps) {
        EXPECT(b.frequency >= 300 && b.frequency <= 2000);
        EXPECT(b.durationMs >= 8 && b.durationMs <= 80);
        EXPECT(b.volume > 0 && b.volume <= 1.0);
        low = low || b.frequency < 700;
        high = high || b.frequency > 900;
    }
    EXPECT(low && high); // Line ends sound different from characters.
}

TEST_CASE("Pips are rate limited, whatever the host's tick rate") {
    for (const long long step : {10, 33, 100}) {
        Heard heard;
        Pc pc(listening(heard));
        pc.run("seq 1 300");
        for (long long t = 0; t < 8000 && pc.sys.revealing(); t += step) {
            pc.advance(step);
        }
        EXPECT(heard.beeps.size() >= 15);
        for (std::size_t i = 1; i < heard.beeps.size(); ++i) {
            const auto gap =
                (heard.beeps[i].at - heard.beeps[i - 1].at).count();
            EXPECT(gap >=
                   std::min<long long>(40, step)); // At most about 25 a second.
        }
    }
}

TEST_CASE("Typing, prompts and idle time are silent") {
    Heard heard;
    Pc pc(listening(heard));
    settle(pc); // The welcome text has its pips.
    heard.beeps.clear();
    pc.type("abc");
    pc.type("\x7f\x7f\x7f");
    pc.type("\r"); // An empty line prints nothing: no pips.
    pc.advance(500);
    EXPECT(heard.beeps.empty());
    pc.advance(2000);
    EXPECT(heard.beeps.empty());
}

TEST_CASE("Skipping the reveal stops the pips") {
    Heard heard;
    Pc pc(listening(heard));
    settle(pc);
    pc.run("seq 1 300");
    pc.advance(300);
    EXPECT(pc.sys.revealing());
    const auto before = heard.beeps.size();
    pc.type(" ");
    EXPECT(!pc.sys.revealing());
    pc.advance(1000);
    EXPECT(heard.beeps.size() == before);
}

TEST_CASE(
    "Sound changes nothing about the text, and the pips are deterministic") {
    auto film = [](bool sound, std::vector<Beep> *out) {
        Heard heard;
        Pc pc(listening(heard, sound));
        pc.run("seq 1 120");
        pc.run("ls");
        settle(pc);
        if (out) {
            *out = heard.beeps;
        }
        return pc.raw;
    };
    std::vector<Beep> first, second;
    const auto loud = film(true, &first);
    const auto again = film(true, &second);
    EXPECT(loud ==
           film(false, nullptr)); // The same bytes with or without sound.
    EXPECT(first.size() == second.size() && !first.empty());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT(first[i].frequency == second[i].frequency &&
               first[i].at == second[i].at);
    }
    EXPECT(loud == again);
}

TEST_CASE("The Matrix types its words with a pip per character") {
    Heard heard;
    Config config = listening(heard, true, 0);
    Pc pc(config);
    pc.run("xyzzy");
    pc.run("/usr/games/plasma");
    pc.advance(100);
    heard.beeps.clear();
    pc.type("r");
    for (int i = 0; i < 170;
         ++i) { // About 5.6 s: the black, then "Hello, Neo.".
        pc.advance(33);
    }
    const auto typed = heard.beeps.size();
    EXPECT(typed >= 9 && typed <= 26); // "Hello, Neo." is 11 characters.
    // Silent while the cursor only blinks.
    const auto count = heard.beeps.size();
    for (int i = 0; i < 20; ++i) {
        pc.advance(33);
        if (pc.shows("Follow")) {
            break;
        }
    }
    EXPECT(heard.beeps.size() <= count + 2);
}

TEST_CASE("The intro types its first line with pips and is silent before") {
    Heard heard;
    Config config = listening(heard, true, 0);
    config.intro = true;
    Pc pc(config);
    for (int i = 0; i < 38;
         ++i) { // t = 1.2 s: the name resolves, no sound yet.
        pc.advance(33);
    }
    EXPECT(heard.beeps.empty());
    for (int i = 0; i < 50; ++i) { // t = 2.9 s: the line is typed (44 letters).
        pc.advance(33);
    }
    EXPECT(heard.beeps.size() >= 20);
    for (const auto &b : heard.beeps) {
        EXPECT(b.frequency >= 300 && b.frequency <= 2000);
    }
}

TEST_SUIT_END
