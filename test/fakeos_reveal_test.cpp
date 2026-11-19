#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

Config slow(int rate = 2000) {
    Config config;
    config.intro = false;
    config.revealCharsPerSecond = rate;
    return config;
}

// How many numbered lines of `seq` are on screen or in the history.
int numbersShown(const Pc &pc) {
    int highest = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        const auto row = pc.row(y);
        if (!row.empty() &&
            row.find_first_not_of("0123456789") == std::string::npos) {
            highest = std::max(highest, std::stoi(row));
        }
    }
    return highest;
}
} // namespace

TEST_SUIT_BEGIN(FakeosReveal)

TEST_CASE("Without a rate everything appears at once") {
    Config config;
    config.intro = false;
    Pc pc(config);
    pc.run("seq 1 60");
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Even a short output is revealed gradually, in a short time") {
    Pc pc(slow(1250));
    pc.run("echo hi");
    EXPECT(pc.sys.revealing());
    EXPECT(
        !pc.shows("\nhi\n")); // Nothing yet: it appears character by character.
    long long elapsed = 0;
    while (pc.sys.revealing() && elapsed < 3000) {
        pc.advance(20);
        elapsed += 20;
    }
    EXPECT(!pc.sys.revealing());
    EXPECT(elapsed >= 240 && elapsed <= 700); // Quick, never sluggish.
    EXPECT(pc.shows("\nhi\n"));
    EXPECT(pc.last() == kPrompt);
    pc.run("ls");
    EXPECT(pc.sys.revealing());
    for (int i = 0; i < 100 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    EXPECT(pc.shows("README"));
}

TEST_CASE("A command with no output only brings the prompt back at once") {
    Pc pc(slow(1250));
    pc.run("true");
    EXPECT(!pc.sys.revealing() || pc.last() == kPrompt);
    pc.run("");
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("The first welcome is revealed, quickly") {
    Pc pc(slow(1250));
    EXPECT(pc.sys.revealing());
    EXPECT(!pc.shows("No real file systems were harmed."));
    pc.advance(60);
    EXPECT(!pc.shows("No real file systems were harmed."));
    long long elapsed = 60;
    while (pc.sys.revealing() && elapsed < 3000) {
        pc.advance(20);
        elapsed += 20;
    }
    EXPECT(elapsed >= 240 && elapsed <= 900);
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.shows("No real file systems were harmed."));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Long output is revealed over time, then the prompt returns") {
    Pc pc(slow(2000));
    pc.run("seq 1 200");
    EXPECT(pc.sys.revealing());
    EXPECT(pc.sys.nextDeadline().has_value()); // The host keeps calling tick.
    EXPECT(pc.last() != kPrompt);
    EXPECT(numbersShown(pc) < 100);
    int previous = numbersShown(pc);
    long long elapsed = 0;
    while (pc.sys.revealing() && elapsed < 6000) {
        pc.advance(50);
        elapsed += 50;
        const int shown = numbersShown(pc);
        EXPECT(shown >= previous ||
               pc.model.historySize() > 0); // Never goes backwards.
        previous = std::max(previous, shown);
    }
    EXPECT(!pc.sys.revealing());
    // About 3000 cost units at 2000 per second.
    EXPECT(elapsed >= 800 && elapsed <= 3000);
    EXPECT(pc.last() == kPrompt);
    EXPECT(!pc.sys.nextDeadline());
    EXPECT(pc.shows("200"));
}

TEST_CASE("The pace does not depend on how often the host ticks") {
    auto total = [](long long step) {
        Pc pc(slow(2000));
        pc.run("seq 1 200");
        long long elapsed = 0;
        while (pc.sys.revealing() && elapsed < 8000) {
            pc.advance(step);
            elapsed += step;
        }
        return elapsed;
    };
    const auto fast = total(16), coarse = total(90);
    EXPECT(std::llabs(fast - coarse) <= 200);
}

TEST_CASE("A frame never carries a burst") {
    Pc pc(slow(2000));
    pc.run("seq 1 400");
    std::size_t biggest = 0;
    for (int i = 0; i < 200 && pc.sys.revealing(); ++i) {
        const auto before = pc.raw.size();
        pc.advance(33);
        biggest = std::max(biggest, pc.raw.size() - before);
    }
    EXPECT(biggest < 300); // About 66 characters per frame, not the whole text.
}

TEST_CASE("Space shows the rest at once and is not typed; other keys are typed "
          "ahead") {
    Pc pc(slow(500));
    pc.run("seq 1 100");
    pc.advance(200);
    EXPECT(pc.sys.revealing());
    pc.type(" ");
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.shows("100"));
    EXPECT(pc.last() == kPrompt); // The skipping key did not land in the line.
    pc.type("ab");
    EXPECT(pc.last() == kPrompt + " ab"); // Typing is never delayed.
    pc.type("\x15");
    pc.run("seq 1 100");
    pc.advance(200);
    pc.type("x"); // Skips and is typed.
    EXPECT(!pc.sys.revealing());
    EXPECT(pc.last() == kPrompt + " x");
    pc.type("\x15");
    pc.run("seq 1 100");
    pc.type("\x03"); // Ctrl-C skips too, without a ^C.
    EXPECT(!pc.sys.revealing());
    EXPECT(!pc.shows("^C"));
}

TEST_CASE("Mouse reports do not skip, and a program waits for the text") {
    Pc pc(slow(1000));
    pc.run("seq 1 100; /usr/games/fire");
    EXPECT(pc.sys.revealing());
    pc.type("\x1b[<0;5;5M");
    EXPECT(pc.sys.revealing());
    EXPECT(pc.raw.find("\x1b[?1049h") == std::string::npos); // Not started yet.
    for (int i = 0; i < 300 && pc.sys.revealing(); ++i) {
        pc.advance(50);
    }
    EXPECT(!pc.sys.revealing());
    pc.advance(100);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.model.alternateScreen());
    pc.type("q");
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("exit waits until its output has been shown") {
    Pc pc(slow(500));
    pc.run("seq 1 60; exit 3");
    EXPECT(pc.sys.revealing());
    EXPECT(!pc.exitCode.has_value());
    for (int i = 0; i < 400 && !pc.exitCode; ++i) {
        pc.advance(50);
    }
    EXPECT(pc.exitCode.has_value());
    EXPECT_EQ(*pc.exitCode, 3);
    EXPECT(pc.shows("60"));
}

TEST_CASE("A very long output is capped at a few seconds") {
    Pc pc(slow(200)); // A slow rate for a big text.
    pc.run("seq 1 3000");
    long long elapsed = 0;
    while (pc.sys.revealing() && elapsed < 20000) {
        pc.advance(100);
        elapsed += 100;
    }
    EXPECT(!pc.sys.revealing());
    EXPECT(elapsed <= 6500);
}

TEST_CASE("Colors and escape sequences cost nothing and arrive whole") {
    Pc pc(slow(100));
    pc.run("ls -l / /etc");
    // Plenty of text with colors; step in tiny pieces and check the stream is
    // never cut inside an escape sequence.
    for (int i = 0; i < 400 && pc.sys.revealing(); ++i) {
        pc.advance(10);
        const auto at = pc.raw.rfind('\x1b');
        if (at != std::string::npos && at + 1 < pc.raw.size() &&
            pc.raw[at + 1] == '[') {
            // A CSI must end with a final byte before the stream ends.
            EXPECT(pc.raw.find_first_of("ABCDEFGHJKSTfmhlnsu", at + 2) !=
                   std::string::npos);
        }
    }
    EXPECT(!pc.sys.revealing());
}

TEST_CASE("The welcome text is revealed too, and the same input gives the same "
          "stream") {
    auto film = [] {
        Config config = slow(1500);
        config.seed = 8;
        Pc pc(config);
        pc.run("seq 1 120");
        for (int i = 0; i < 100; ++i) {
            pc.advance(40);
        }
        return pc.raw;
    };
    EXPECT(film() == film());
}

TEST_SUIT_END
