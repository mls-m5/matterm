#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

int litCells(const Pc &pc) {
    int n = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
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
} // namespace

TEST_SUIT_BEGIN(FakeosToys)

TEST_CASE("snow falls, piles up and q leaves with a line") {
    Pc pc(false, 80, 24, 5);
    pc.run("snow");
    EXPECT(pc.sys.inProgram());
    pc.advance(200);
    EXPECT(pc.shows("snow   q quit"));
    runFor(pc, 20000);
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("The snow melts"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("life is alive, reseeds on space and ends on q") {
    Pc pc(false, 60, 20, 3);
    pc.run("/usr/games/life");
    pc.advance(200);
    EXPECT(pc.shows("life   q quit"));
    EXPECT(litCells(pc) > 100);
    runFor(pc, 3000);
    EXPECT(pc.sys.inProgram());
    pc.type(" ");
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Life goes on."));
}

TEST_CASE("sl: the train crosses and leaves by itself") {
    Pc pc(false, 80, 24, 2);
    pc.run("sl");
    EXPECT(pc.sys.inProgram());
    runFor(pc, 1500);
    EXPECT(litCells(pc) > 50);
    runFor(pc, 12000);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("You meant ls"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("clock: a big clock, space switches to 12 hours, q leaves") {
    Pc pc(false, 80, 24, 2);
    pc.run("clock");
    EXPECT(pc.sys.inProgram());
    runFor(pc, 1200);
    EXPECT(pc.shows("UTC   q quit"));
    EXPECT(litCells(pc) > 60); // Big digits.
    pc.type(" ");
    runFor(pc, 300);
    EXPECT(pc.shows("AM   q quit"));
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Time flies."));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("starfield: stars move, speed keys work, q leaves") {
    Pc pc(false, 80, 24, 4);
    pc.run("/usr/games/starfield");
    runFor(pc, 800);
    EXPECT(pc.shows("starfield   q quit"));
    const int before = litCells(pc);
    EXPECT(before > 30);
    pc.type("+");
    pc.type("-");
    runFor(pc, 2000);
    EXPECT(litCells(pc) > 30);
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(pc.shows("Warp drive disengaged."));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("aquarium: fish swim, space feeds them, q leaves") {
    Pc pc(false, 80, 24, 6);
    pc.run("aquarium");
    EXPECT(pc.sys.inProgram()); // It lives in /usr/games, which is in the PATH.
    runFor(pc, 1500);
    EXPECT(pc.shows("aquarium   q quit"));
    EXPECT(pc.shows("><") || pc.shows("<>") || pc.shows("))>") ||
           pc.shows("<(("));
    pc.type(" ");
    runFor(pc, 1500);
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(pc.shows("The fish have already forgotten you."));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("More small eggs: ping, ssh, curl, git, python, apt") {
    Pc pc;
    pc.run("ping example.org");
    EXPECT(pc.shows("The network is a rumor here"));
    pc.run("clear");
    pc.run("ssh me@elsewhere");
    EXPECT(pc.shows("no network. It has opinions"));
    pc.run("clear");
    pc.run("curl https://example.org");
    EXPECT(pc.shows("There is only this."));
    pc.run("clear");
    pc.run("git status");
    EXPECT(pc.shows("not a git repository"));
    pc.run("clear");
    pc.run("python3 -c 1");
    EXPECT(pc.shows("vibes"));
    pc.run("clear");
    pc.run("apt install reality");
    EXPECT(pc.shows("Unable to locate package reality"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("the small courtesies") {
    Pc pc;
    pc.run("please");
    EXPECT(pc.shows("Polite!"));
    pc.run("make coffee");
    EXPECT(pc.shows("418"));
}

TEST_SUIT_END
