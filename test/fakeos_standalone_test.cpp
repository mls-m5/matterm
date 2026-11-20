#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

using fakeos_test::Pc;

namespace {
fakeos::Config standalone(bool intro) {
    fakeos::Config c;
    c.standalone = true;
    c.hostname = "mattrix";
    c.intro = intro;
    c.seed = 4;
    return c;
}
} // namespace

TEST_SUIT_BEGIN(FakeosStandalone)

TEST_CASE("The standalone intro is Mattrix, never matterm") {
    Pc pc(standalone(true));
    for (int t = 0; t < 3000; t += 33) {
        pc.advance(33);
    }
    EXPECT(pc.shows("Mattrix OS 6.0 Professional Edition"));
    EXPECT(!pc.shows("Futuristic"));
    EXPECT(!pc.shows("terminal emulator"));
    for (int t = 0; t < 3500; t += 33) {
        pc.advance(33);
    }
    EXPECT(pc.shows("A fake operating system in your real terminal"));
}

TEST_CASE("The demo's intro still names matterm") {
    fakeos::Config c = standalone(true);
    c.standalone = false;
    Pc pc(c);
    for (int t = 0; t < 3000; t += 33) {
        pc.advance(33);
    }
    EXPECT(pc.shows("Futuristic system agnostic terminal emulator"));
}

TEST_CASE("The prompt and about speak as Mattrix OS") {
    Pc pc(standalone(false));
    EXPECT(pc.shows("guest@mattrix"));
    pc.run("about | head -6");
    EXPECT(pc.shows("runs in your real"));
    EXPECT(!pc.shows("demo: a real terminal"));
}

TEST_SUIT_END
