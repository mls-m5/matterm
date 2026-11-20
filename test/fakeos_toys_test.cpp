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

TEST_CASE("the small courtesies") {
    Pc pc;
    pc.run("please");
    EXPECT(pc.shows("Polite!"));
    pc.run("make coffee");
    EXPECT(pc.shows("418"));
}

TEST_SUIT_END
