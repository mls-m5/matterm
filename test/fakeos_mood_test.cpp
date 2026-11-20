#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>
#include <vector>

// Saying hello again and again makes the system grumpier, until it reboots.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

const char *kSteps[] = {
    "Hello, guest. The system hums quietly.",
    "Hello again, guest.",
    "Again? Did you forget?",
    "Is this a test?",
    "Please stop.",
    "I am a Professional Edition. Please act professionally.",
    "I can do this all day. Can you?",
    "please kill me...",
};
} // namespace

namespace {
// The line just above the prompt: the last answer.
std::string answer(const Pc &pc) {
    return pc.row(pc.model.cursor().row - 1);
}
} // namespace

TEST_SUIT_BEGIN(FakeosMood)

TEST_CASE("Each hello gets a grumpier answer, hi and hej count too") {
    Pc pc(false, 100, 60);
    const char *words[] = {"hello", "hi", "hej", "hello", "hi", "hej", "hello"};
    for (int i = 0; i < 7; ++i) {
        pc.run(words[i]);
        EXPECT(answer(pc) == kSteps[i]);
    }
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("After the last step it begs, reboots and plays the intro again") {
    Pc pc(false, 100, 60);
    pc.run("echo souvenir > keepsake");
    for (int i = 0; i < 7; ++i) {
        pc.run("hello");
    }
    EXPECT(!pc.sys.inProgram());
    pc.run("hello"); // The eighth.
    // The intro takes the screen at once; the words went by before it.
    EXPECT(pc.raw.find("please kill me...") != std::string::npos);
    EXPECT(pc.raw.find("Rebooting the system...") != std::string::npos);
    EXPECT(pc.sys.inProgram()); // The intro.
    pc.advance(300);
    pc.type("x");
    pc.advance(1400);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.last() == kPrompt);
    // Fresh world: the file is gone, the mood is calm again.
    pc.run("ls");
    EXPECT(!pc.shows("keepsake"));
    pc.run("hello");
    EXPECT(answer(pc) == kSteps[0]);
}

TEST_CASE("Another command in between calms it a little") {
    Pc pc(false, 100, 60);
    pc.run("hello");
    pc.run("hello");
    pc.run("hello"); // The mood is 3: the next answer is step 3.
    pc.run("ls");    // One step calmer.
    pc.run("hello");
    EXPECT(answer(pc) == kSteps[2]);
    pc.run("hello");
    EXPECT(answer(pc) == kSteps[3]);
    // Many other commands calm it completely, never below the start.
    for (int i = 0; i < 12; ++i) {
        pc.run("true");
    }
    pc.run("hello");
    EXPECT(answer(pc) == kSteps[0]);
}

TEST_CASE("Empty lines do not count as other commands") {
    Pc pc(false, 100, 60);
    pc.run("hello");
    pc.run("hello");
    pc.run("");
    pc.run("");
    pc.run("hello");
    EXPECT(answer(pc) == kSteps[2]);
}

TEST_CASE("It is deterministic, and the answers are revealed with the rest") {
    auto film = [] {
        Config config;
        config.intro = false;
        config.revealCharsPerSecond = 1250;
        Pc pc(config);
        for (int i = 0; i < 100; ++i) {
            pc.advance(20);
        }
        for (int round = 0; round < 3; ++round) {
            pc.run("hello");
            for (int i = 0; i < 60; ++i) {
                pc.advance(20);
            }
        }
        return pc.raw;
    };
    EXPECT(film() == film());
}

TEST_SUIT_END
