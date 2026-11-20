#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <set>
#include <string>

// rm -rf / warns; with --no-preserve-root it asks; yes crashes the system
// (a boot failure with a blinking cursor, no prompt) and it restarts by itself.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";
const std::string kQuestion = "Are you really really sure? (y/n)";

void walk(Pc &pc, long long ms) {
    for (long long t = 0; t < ms; t += 33) {
        pc.advance(33);
    }
}

// Ask the question and answer yes.
void crash(Pc &pc, const char *line = "rm -rf --no-preserve-root /") {
    pc.run(line);
    pc.type("y");
    walk(pc, 100);
}

int litCells(const Pc &pc) {
    int n = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            n += pc.model.cell(x, y).codePoint != U' ';
        }
    }
    return n;
}
} // namespace

TEST_SUIT_BEGIN(FakeosCrash)

TEST_CASE("Plain rm -rf / still just warns") {
    Pc pc;
    pc.run("rm -rf /");
    EXPECT(pc.shows("rm: it is dangerous to operate recursively on '/'"));
    EXPECT(pc.shows("use --no-preserve-root"));
    EXPECT(pc.last() == kPrompt);
    EXPECT(pc.sys.world().fs.find("/etc") != nullptr);
}

TEST_CASE("With --no-preserve-root it asks first, with or without sudo") {
    for (const char *line : {"rm -rf --no-preserve-root /",
                             "sudo rm -rf --no-preserve-root /",
                             "rm -rf / --no-preserve-root",
                             "rm -fr --no-preserve-root /",
                             "sudo rm -rf / --no-preserve-root"}) {
        Pc pc;
        pc.run(line);
        EXPECT(pc.shows(kQuestion));
        EXPECT(pc.last().find(kQuestion) !=
               std::string::npos); // No prompt: it waits.
        EXPECT(!pc.sys.inProgram());
        EXPECT(pc.sys.world().fs.find("/etc") !=
               nullptr); // Nothing happened yet.
    }
}

TEST_CASE("Anything but y backs out: Wise choice.") {
    for (const char *answer : {"n", "N", "\r", "x", " "}) {
        Pc pc;
        pc.run("rm -rf --no-preserve-root /");
        pc.type(answer);
        EXPECT(pc.shows("Wise choice."));
        EXPECT(pc.last() == kPrompt);
        EXPECT(pc.sys.world().fs.find("/etc") != nullptr);
        pc.run("ls /");
        EXPECT(pc.shows("etc"));
    }
}

TEST_CASE("Other rm commands are not part of it") {
    Pc pc;
    pc.run("mkdir -p junk/a");
    pc.run("rm -rf junk");
    EXPECT(!pc.shows(kQuestion));
    pc.run("rm -rf /tmp");
    EXPECT(!pc.shows(kQuestion)); // Permission denied, as ever.
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE(
    "Yes: the screen corrupts first, then clears, then the machine hangs") {
    Pc pc(false, 80, 24);
    pc.run("echo keep > keepsake");
    crash(pc);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.model.alternateScreen());
    EXPECT(pc.sys.world().fs.find("/etc") == nullptr); // Everything is gone.
    EXPECT(pc.sys.world().fs.find("/home") == nullptr);
    const int total = 80 * 24;
    EXPECT(litCells(pc) > 0 &&
           litCells(pc) < total / 4); // It starts to go wrong.
    walk(pc, 1000);
    const int midway = litCells(pc);
    EXPECT(midway > total / 4);
    const auto first = pc.screen();
    walk(pc, 120);
    EXPECT(pc.screen() != first); // The glyphs keep changing.
    walk(pc, 800);
    EXPECT(litCells(pc) > total * 6 / 10); // Garbage nearly everywhere.
    // Random colors, some cells inverted (a bright background).
    int colors = 0, inverted = 0;
    std::set<unsigned> seen;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            const auto &cell = pc.model.cell(x, y);
            if (cell.codePoint == U' ')
                continue;
            seen.insert(unsigned(cell.foreground.r) << 16 |
                        unsigned(cell.foreground.g) << 8 | cell.foreground.b);
            inverted +=
                cell.background.r + cell.background.g + cell.background.b > 300;
        }
    }
    colors = int(seen.size());
    EXPECT(colors > 20);
    EXPECT(inverted > 20);
    walk(pc, 600); // t = about 2.6 s: black.
    EXPECT(litCells(pc) < 5);
    walk(pc, 1500);
    EXPECT(pc.shows("Looking for boot media..."));
    EXPECT(!pc.shows("No bootable device"));
    walk(pc, 3000);
    EXPECT(
        pc.shows("No bootable device -- insert boot disk and press any key"));
    EXPECT(!pc.shows(kPrompt)); // No prompt, nothing to type into.
    // A blinking cursor.
    bool on = false, off = false;
    for (int i = 0; i < 40; ++i) {
        pc.advance(50);
        const bool cursor = pc.shows("press any key_");
        on = on || cursor;
        off = off || !cursor;
    }
    EXPECT(on && off);
    EXPECT(pc.sys.inProgram());
}

TEST_CASE("The corruption is seeded: same seed, same garbage") {
    auto garbage = [](std::uint64_t seed) {
        Pc pc(false, 60, 18, seed);
        crash(pc);
        walk(pc, 900);
        return pc.screen();
    };
    EXPECT(garbage(3) == garbage(3));
    EXPECT(garbage(3) != garbage(4));
}

TEST_CASE(
    "Keys before the message are ignored; one after it restarts at once") {
    Pc pc;
    crash(pc);
    walk(pc, 600);
    pc.type("a");
    pc.type("\r");
    walk(pc, 300);
    EXPECT(pc.sys.inProgram());
    EXPECT(!pc.shows("No bootable device"));
    walk(pc, 4800); // t = about 5.8 s: the message is up.
    EXPECT(pc.shows("No bootable device"));
    pc.type("x"); // Any key.
    walk(pc, 200);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.shows("press any key to skip")); // The boot sequence: the intro.
    pc.type("x");
    walk(pc, 1500);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.last() == kPrompt);
    pc.run("ls /");
    EXPECT(pc.shows("etc")); // A new world.
    pc.run("ls");
    EXPECT(!pc.shows("keepsake"));
}

TEST_CASE("Left alone it restarts by itself after about nine seconds") {
    Pc pc;
    crash(pc);
    walk(pc, 7500);
    EXPECT(pc.shows("No bootable device")); // Still hung well into it.
    EXPECT(!pc.shows("press any key to skip"));
    walk(pc, 3000); // Ten and a half seconds in all.
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.shows("press any key to skip")); // The intro has begun.
    walk(pc, 8000);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.last() == kPrompt);
    pc.run("echo back");
    EXPECT(pc.shows("back"));
}

TEST_CASE("The crash is deterministic, also with the paced reveal and sound") {
    auto film = [] {
        Config config;
        config.intro = false;
        config.revealCharsPerSecond = 1250;
        config.sound = true;
        config.onBeep = [](const Beep &) {};
        Pc pc(config);
        for (int i = 0; i < 100; ++i) {
            pc.advance(20);
        }
        pc.run("sudo rm -rf --no-preserve-root /");
        for (int i = 0; i < 60; ++i) {
            pc.advance(20);
        }
        pc.type("y");
        for (int i = 0; i < 400; ++i) {
            pc.advance(33);
        }
        return pc.raw;
    };
    EXPECT(film() == film());
}

TEST_CASE("The question is revealed with the rest and y can be typed ahead") {
    Config config;
    config.intro = false;
    config.revealCharsPerSecond = 1250;
    Pc pc(config);
    for (int i = 0; i < 100 && pc.sys.revealing(); ++i) {
        pc.advance(20);
    }
    pc.run("rm -rf --no-preserve-root /");
    pc.type(
        "y"); // Before the question is out: it skips the reveal and answers.
    walk(pc, 100);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.sys.world().fs.find("/etc") == nullptr);
}

TEST_SUIT_END
