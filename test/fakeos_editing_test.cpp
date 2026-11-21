#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

using fakeos_test::Pc;

TEST_SUIT_BEGIN(FakeosEditing)

TEST_CASE("The prompt deletes and moves by words, like a shell") {
    Pc pc;
    pc.type("echo one two three");
    pc.type("\x1b\x7f"); // Alt+Backspace
    pc.type("\r");
    EXPECT(pc.shows("one two"));
    EXPECT(!pc.shows("three"));
    pc.run("clear");
    pc.type("echo aaa bbb ccc");
    pc.type("\x1b[1;5D"); // Ctrl+Left: before ccc
    pc.type("X");
    pc.type("\x1b[1;5D\x1b[1;5D\x1b[1;5D"); // Before the X, bbb, then aaa.
    pc.type("\x1b"
            "d"); // Alt+D deletes "aaa"
    pc.type("\r");
    EXPECT(pc.shows("bbb Xccc"));
    EXPECT(!pc.shows("aaa"));
    pc.run("clear");
    pc.type("echo first second third");
    pc.type("\x1b[1;5D\x1b[1;5D"); // Before "second".
    pc.type("\x1b[3;5~");          // Ctrl+Delete removes "second".
    pc.type("\r");
    EXPECT(pc.shows("first third"));
    pc.run("clear");
    pc.type("echo one two");
    pc.type("\x1b"
            "b\x1b"
            "b\x1b"
            "bQ"); // Alt+B three times: before "echo"; then a letter.
    pc.type("\x1b"
            "f"); // Alt+F: after "Qecho".
    pc.type("\r");
    EXPECT(pc.shows("command not found")); // "Qecho one two" is not a command.
    EXPECT(pc.shows("Qecho"));
}

TEST_CASE("Ctrl-W still works and plain keys are unchanged") {
    Pc pc;
    pc.type("echo one two");
    pc.type("\x17");
    pc.type("\r");
    EXPECT(pc.shows("one"));
    EXPECT(!pc.shows("two"));
    pc.run("clear");
    pc.type("echo ab");
    pc.type("\x1b[D\x1b[3~"); // Left, then Delete: removes "b".
    pc.type("\r");
    EXPECT(pc.shows("a"));
}

TEST_SUIT_END
