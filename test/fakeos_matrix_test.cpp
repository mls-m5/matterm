#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

// Lit cells (not blank) in rows [from, to).
int litCells(const Pc &pc, std::size_t from, std::size_t to) {
    int n = 0;
    for (std::size_t y = from; y < to && y < pc.model.rows(); ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            const auto cp = pc.model.cell(x, y).codePoint;
            n += cp != U' ' && cp != 0;
        }
    }
    return n;
}

void runFor(Pc &pc, long long ms, long long step = 33) {
    for (long long t = 0; t < ms; t += step) {
        pc.advance(step);
    }
}

// Reach the red pill: the whole discovery chain, as a visitor would.
void toThePills(Pc &pc) {
    pc.run("xyzzy");
    pc.run("/usr/games/plasma");
    pc.advance(100);
}
} // namespace

TEST_SUIT_BEGIN(FakeosMatrix)

TEST_CASE("The shy program is hidden by its dot") {
    Pc pc;
    pc.run("ls /usr/games");
    EXPECT(pc.shows("fire"));
    EXPECT(!pc.shows("crumble"));
    pc.run("clear");
    pc.run("ls -a /usr/games");
    EXPECT(pc.shows(".crumble"));
    pc.run("cat ~/.plan");
    EXPECT(pc.shows("some are shy: ls -a"));
}

TEST_CASE("crumble: a word stands, cracks, falls and piles up") {
    Pc pc(false, 80, 24, 9);
    pc.run("/usr/games/.crumble");
    EXPECT(pc.sys.inProgram());
    pc.advance(300);
    const auto topAtStart = litCells(pc, 0, 12);
    EXPECT(topAtStart > 20); // The big letters.
    EXPECT(pc.shows("crumble   q quit"));
    runFor(pc, 12000);
    EXPECT(litCells(pc, 0, 12) == 0);  // Nothing left up there.
    EXPECT(litCells(pc, 12, 22) > 20); // A pile on the floor.
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.sys.world().flags.count("crumbled") == 1);
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("crumble is deterministic, takes a word and plays again on space") {
    auto film = [](std::uint64_t seed, const char *command) {
        Pc pc(false, 60, 16, seed);
        pc.run(command);
        runFor(pc, 6000);
        return pc.raw;
    };
    EXPECT(film(4, "/usr/games/.crumble") == film(4, "/usr/games/.crumble"));
    EXPECT(film(4, "/usr/games/.crumble") != film(5, "/usr/games/.crumble"));
    Pc pc(false, 60, 16, 4);
    pc.run("/usr/games/.crumble HI");
    pc.advance(300);
    const auto word = litCells(pc, 0, 8);
    EXPECT(word > 10 && word < 60); // Two letters, not seven.
    runFor(pc, 9000);
    pc.type(" "); // Again.
    pc.advance(300);
    EXPECT(litCells(pc, 0, 8) > 10);
}

TEST_CASE("The red pill starts the Matrix: black, typed words, a cursor") {
    Pc pc;
    toThePills(pc);
    pc.type("r");
    EXPECT(pc.shows(""));       // Plasma ended...
    EXPECT(pc.sys.inProgram()); // ...and the next program started.
    pc.advance(100);
    EXPECT(litCells(pc, 0, 24) == 0); // The screen is black.
    runFor(pc, 1700);                 // Typing "Hello, Neo." has begun.
    EXPECT(pc.shows("Hel"));
    EXPECT(!pc.shows("Hello, Neo."));
    runFor(pc, 1200);
    EXPECT(pc.shows("Hello, Neo."));
    // The cursor blinks while the line waits.
    bool seenOn = false, seenOff = false;
    for (int i = 0; i < 40; ++i) {
        pc.advance(50);
        const bool cursor = pc.shows("Hello, Neo._");
        seenOn = seenOn || cursor;
        seenOff = seenOff || !cursor;
    }
    EXPECT(seenOn && seenOff);
}

TEST_CASE("After a pause the white rabbit, then the rain") {
    Pc pc;
    toThePills(pc);
    pc.type("r");
    runFor(pc, 3800); // The first line is done and held.
    runFor(pc, 1500);
    EXPECT(!pc.shows("Hello, Neo."));
    runFor(pc, 3300);
    EXPECT(pc.shows("Follow the white rabbit."));
    runFor(pc, 3500); // Rain.
    EXPECT(!pc.shows("white rabbit"));
    EXPECT(litCells(pc, 0, 24) > 100);
    // Green characters with a bright head.
    int green = 0, bright = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            const auto &cell = pc.model.cell(x, y);
            if (cell.codePoint == U' ')
                continue;
            green += cell.foreground.g > cell.foreground.r + 30 &&
                     cell.foreground.g > cell.foreground.b;
            bright += cell.foreground.r > 180 && cell.foreground.g > 230;
        }
    }
    EXPECT(green > 50);
    EXPECT(bright > 3);
}

TEST_CASE("The rain runs out and the intro follows, then the prompt; the world "
          "is kept") {
    Pc pc;
    pc.run("echo kept > souvenir");
    toThePills(pc);
    pc.type("r");
    runFor(pc, 30000);
    // By now the Matrix is over and the intro has been playing or finished.
    EXPECT(pc.sys.world().flags.count("matrix-seen") == 1);
    runFor(pc, 6000);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.last() == kPrompt);
    pc.run("cat souvenir");
    EXPECT(pc.shows("kept"));
    pc.run("ls /usr/games");
    EXPECT(pc.shows("plasma")); // Discoveries remain.
}

TEST_CASE("A key skips part by part: words, then rain, then the intro") {
    Pc pc;
    toThePills(pc);
    pc.type("r");
    pc.advance(200);
    pc.type("x"); // Skips the black.
    pc.advance(100);
    pc.type("x"); // The first line.
    pc.advance(100);
    pc.type("x"); // The second line: now the rain.
    runFor(pc, 1500);
    EXPECT(litCells(pc, 0, 24) > 50);
    EXPECT(!pc.shows("Hello"));
    pc.type("x"); // Skips the rain: the intro plays.
    pc.advance(300);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.shows("press any key to skip"));
    pc.type("x"); // Skips the intro: the prompt is back.
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to matterm 1.0"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Clicking the red word works like the key") {
    Pc pc;
    toThePills(pc);
    int column = -1, row = -1;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        const auto at = pc.row(y).find("red");
        if (at != std::string::npos &&
            pc.row(y).find("( red )") != std::string::npos) {
            column = int(at);
            row = int(y);
        }
    }
    EXPECT(column >= 0);
    pc.type("\x1b[<0;" + std::to_string(column + 2) + ";" +
            std::to_string(row + 1) + "M");
    EXPECT(pc.sys.inProgram());
    pc.advance(100);
    EXPECT(litCells(pc, 0, 24) == 0); // Black: the Matrix began.
    EXPECT(pc.sys.world().flags.count("pill-red") == 1);
}

TEST_CASE("The sequence is deterministic for a seed") {
    auto film = [](std::uint64_t seed) {
        Pc pc(false, 60, 18, seed);
        toThePills(pc);
        pc.type("r");
        runFor(pc, 13000);
        return pc.raw;
    };
    EXPECT(film(11) == film(11));
    EXPECT(film(11) != film(12));
}

TEST_CASE("The rain follows a resize") {
    Pc pc(false, 60, 18);
    toThePills(pc);
    pc.type("r");
    runFor(pc, 12500);
    pc.sys.resize(40, 12);
    pc.model.resize(40, 12);
    runFor(pc, 2000);
    EXPECT(pc.sys.inProgram());
    EXPECT(litCells(pc, 0, 12) > 20);
    pc.sys.resize(90, 30);
    pc.model.resize(90, 30);
    runFor(pc, 2000);
    EXPECT(litCells(pc, 0, 30) > 30);
}

TEST_SUIT_END
