#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

// Random terminal input: text, control keys, escape sequences, mouse reports,
// commands that start programs, and now and then a resize.
std::string noise(Rng &rng) {
    static const char *pieces[] = {
        "a",
        "Z",
        " ",
        "|",
        ">",
        "<",
        ";",
        "&&",
        "$HOME",
        "'",
        "\"",
        "*",
        "?",
        "~",
        "\\",
        "\r",
        "\t",
        "\x7f",
        "\x03",
        "\x04",
        "\x15",
        "\x1b",
        "\x1b[A",
        "\x1b[B",
        "\x1b[C",
        "\x1b[D",
        "\x1b[3~",
        "\x1b[H",
        "\x1b[F",
        "\x1b[<0;5;5M",
        "\x1b[<64;9;3M",
        "\xc3\xa5",
        "\xe2\x82\xac",
        "\xf0\x9f\x94\xa5",
        "\xc3",
        "\x80",
        "ls\r",
        "cat README\r",
        "seq 1 300\r",
        "yes | head -n 5\r",
        "rm -r *\r",
        "echo $?\r",
        "/usr/games/fire\r",
        "less README\r",
        "top\r",
        "xyzzy\r",
        "/usr/games/plasma\r",
        "/usr/games/.crumble\r",
        "reset\r",
        "history\r",
        "cal\r",
        "grep -n e /usr/share/dict/words\r",
    };
    std::string out;
    const auto count = 1 + rng.below(6);
    for (std::uint32_t i = 0; i < count; ++i) {
        out += pieces[rng.below(sizeof pieces / sizeof pieces[0])];
    }
    return out;
}
} // namespace

TEST_SUIT_BEGIN(FakeosRobust)

TEST_CASE("Random input never crashes the system, and it stays usable") {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        Pc pc(false, 70, 20, seed);
        Rng rng(seed * 7919);
        std::size_t biggest = 0;
        for (int i = 0; i < 1500; ++i) {
            const auto before = pc.raw.size();
            pc.type(noise(rng));
            pc.advance(int(rng.below(120)));
            if (rng.below(150) == 0) {
                const int columns = 20 + int(rng.below(100)),
                          rows = 6 + int(rng.below(40));
                pc.sys.resize(columns, rows);
                pc.model.resize(std::size_t(columns), std::size_t(rows));
            }
            biggest = std::max(biggest, pc.raw.size() - before);
        }
        // Output per step stays bounded (a `seq 1 300` or a frame, not a
        // flood).
        EXPECT(biggest < 1500000);
        // Get out of whatever is running, then the shell still works.
        for (int i = 0; i < 40 && pc.sys.inProgram(); ++i) {
            pc.type("q\x03\x1b ");
            pc.advance(100);
        }
        for (int i = 0; i < 40 && pc.sys.inProgram(); ++i) {
            pc.type("x");
            pc.advance(400);
        }
        EXPECT(!pc.sys.inProgram());
        pc.type("\x15");
        pc.run("echo still alive");
        EXPECT(pc.shows("still alive") || pc.sys.world().history.size() > 0);
    }
}

TEST_CASE("The same noise gives the same output") {
    auto run = [](std::uint64_t seed) {
        Pc pc(false, 60, 18, seed);
        Rng rng(99);
        for (int i = 0; i < 300; ++i) {
            pc.type(noise(rng));
            pc.advance(int(rng.below(80)));
        }
        return pc.raw;
    };
    EXPECT(run(5) == run(5));
}

TEST_CASE("Runaway files and directories hit the limits with proper errors") {
    Pc pc;
    pc.run("seq 1 100000 > big");
    EXPECT(pc.shows("File too large"));
    pc.run("clear");
    pc.run("yes | head -n 1000 > f");
    pc.run("cat f f f f f f f f > g");
    pc.run("cat g g g g g g g g > h");
    pc.run("clear");
    pc.run("cat h h h h h h h h > i");
    EXPECT(pc.shows("File too large"));
    pc.run("clear");
    pc.run("echo x > a; for=1; ls");
    EXPECT(pc.shows("a"));
    // Deep trees stop at the depth limit (32 levels, a few at a time).
    for (int round = 0; round < 4; ++round) {
        pc.run("mkdir -p d/d/d/d/d/d/d/d/d/d");
        pc.run("cd d/d/d/d/d/d/d/d/d/d");
    }
    pc.run("clear");
    pc.run("mkdir -p d/d/d/d/d/d/d/d/d/d");
    EXPECT(pc.shows("Invalid argument"));
    // The system keeps working.
    pc.run("clear");
    pc.run("echo fine");
    EXPECT(pc.shows("fine"));
}

TEST_CASE("Paste with newlines runs line by line") {
    Pc pc;
    pc.type("\x1b[200~echo one\necho two\n\x1b[201~");
    EXPECT(pc.shows("\none\n"));
    EXPECT(pc.shows("\ntwo\n"));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("A zero or tiny terminal does not break anything") {
    Pc pc(false, 20, 6);
    pc.run("ls");
    pc.run("help");
    pc.sys.resize(1, 1);
    pc.model.resize(1, 1);
    pc.type("abc\r");
    pc.run("/usr/games/fire");
    pc.advance(200);
    pc.type("q");
    pc.sys.resize(80, 24);
    pc.model.resize(80, 24);
    pc.run("echo recovered");
    EXPECT(pc.shows("recovered"));
}

TEST_SUIT_END
