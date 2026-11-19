#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";
} // namespace

TEST_SUIT_BEGIN(FakeosMoreCommands)

TEST_CASE("cal prints the pretend month, any month and a year") {
    Pc pc;
    pc.run("cal");
    EXPECT(pc.shows("January 2026"));
    EXPECT(pc.shows("Su Mo Tu We Th Fr Sa"));
    EXPECT(pc.shows("             1  2  3")); // 2026-01-01 is a Thursday.
    EXPECT(pc.shows(" 4  5  6  7  8  9 10"));
    pc.run("clear");
    pc.run("cal 2 2024");
    EXPECT(pc.shows("February 2024"));
    EXPECT(pc.shows("29"));
    pc.run("clear");
    pc.run("cal 2 2023");
    EXPECT(!pc.shows("29"));
    pc.run("cal 13 2024");
    EXPECT(pc.shows("cal: invalid date"));
}

TEST_CASE("fortune is deterministic for a seed") {
    Pc a(false, 80, 24, 5), b(false, 80, 24, 5);
    a.run("fortune");
    b.run("fortune");
    EXPECT(a.screen() == b.screen());
    EXPECT(a.row(a.model.cursor().row - 1).size() > 5);
}

TEST_CASE("cowsay wraps the text in a bubble") {
    Pc pc;
    pc.run("cowsay hi");
    EXPECT(pc.shows("< hi >"));
    EXPECT(pc.shows("(oo)"));
    pc.run("clear");
    pc.run("echo piped words | cowsay");
    EXPECT(pc.shows("< piped words >"));
}

TEST_CASE("Text filters: rev, tac, nl, tr, cut, sed, printf") {
    Pc pc;
    pc.run("echo abc | rev");
    EXPECT(pc.shows("cba"));
    pc.run("seq 1 3 | tac | tr -d '\\n'");
    pc.run("echo");
    EXPECT(pc.shows("321"));
    pc.run("clear");
    pc.run("echo hello | tr a-z A-Z");
    EXPECT(pc.shows("HELLO"));
    pc.run("echo a:b:c | cut -d: -f2");
    EXPECT(pc.shows("\nb\n"));
    pc.run("echo abcdef | cut -c2-4");
    EXPECT(pc.shows("bcd"));
    pc.run("clear");
    pc.run("echo foo foo | sed s/foo/bar/");
    EXPECT(pc.shows("bar foo"));
    pc.run("echo foo foo | sed s/foo/bar/g");
    EXPECT(pc.shows("bar bar"));
    pc.run("printf '%s-%d\\n' a 5");
    EXPECT(pc.shows("a-5"));
    pc.run("clear");
    pc.run("echo x | nl");
    const auto row = pc.row(pc.model.cursor().row - 1);
    EXPECT(row.find("     1") == 0 && row.back() == 'x');
    pc.run("echo x | sed bogus");
    EXPECT(pc.shows("only s/pattern/replacement/"));
}

TEST_CASE("Paths and system information commands") {
    Pc pc;
    pc.run("basename /a/b/c.txt .txt");
    EXPECT(pc.shows("\nc\n"));
    pc.run("dirname /a/b/c.txt");
    EXPECT(pc.shows("\n/a/b\n"));
    pc.run("clear");
    pc.run("df");
    EXPECT(pc.shows("pretendfs"));
    pc.run("free");
    EXPECT(pc.shows("Mem:"));
    pc.run("sleep 100; echo awake");
    EXPECT(pc.shows("awake")); // Nothing waits in a pretend system.
}

TEST_CASE("less pages a file on the alternate screen") {
    Pc pc;
    pc.run("seq 1 100 > numbers");
    pc.run("less numbers");
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.model.alternateScreen());
    pc.advance(50);
    EXPECT(pc.row(0) == "1");
    EXPECT(pc.row(1) == "2");
    EXPECT(pc.shows(":"));
    pc.type(" "); // A page forward.
    pc.advance(50);
    EXPECT(pc.row(0) != "1");
    pc.type("G");
    pc.advance(50);
    EXPECT(pc.shows("100"));
    EXPECT(pc.shows("(END)"));
    pc.type("g");
    pc.advance(50);
    EXPECT(pc.row(0) == "1");
    pc.type("\x1b[<65;5;5M"); // Wheel down.
    pc.advance(50);
    EXPECT(pc.row(0) == "4");
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("less reads a pipe and reports a missing file") {
    Pc pc;
    pc.run("echo from a pipe | less");
    EXPECT(pc.sys.inProgram());
    pc.advance(50);
    EXPECT(pc.row(0) == "from a pipe");
    pc.type("q");
    pc.run("less /nope");
    EXPECT(pc.shows("less: /nope: No such file or directory"));
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE("top shows moving processes and q quits") {
    Pc pc;
    pc.run("top");
    EXPECT(pc.sys.inProgram());
    pc.advance(50);
    EXPECT(pc.shows("PID"));
    EXPECT(pc.shows("matterm-sh"));
    const auto first = pc.screen();
    pc.advance(1200);
    EXPECT(pc.screen() != first);
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE("More eggs") {
    Pc pc;
    pc.run("vim");
    EXPECT(pc.shows("There is no editor here"));
    pc.run("42");
    EXPECT(pc.shows("The question is still open."));
    pc.run("neo");
    EXPECT(pc.shows("The Matrix has you."));
    pc.run("rm -rf --no-preserve-root /");
    EXPECT(pc.shows("Nice try."));
    pc.run(":(){ :|:& };:");
    EXPECT(pc.shows("Resource temporarily unavailable"));
}

TEST_SUIT_END
