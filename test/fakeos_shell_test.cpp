#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";
} // namespace

TEST_SUIT_BEGIN(FakeosShell)

TEST_CASE("Without the intro the system opens with the motd and a prompt") {
    Pc pc;
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.last() == kPrompt);
    EXPECT(!pc.sys.nextDeadline()); // Idle: nothing wakes the host.
}

TEST_CASE("The welcome is short, friendly and points the way") {
    Pc pc;
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(!pc.shows("pretend"));
    EXPECT(!pc.shows("GNU/Linux"));
    EXPECT(!pc.shows("x86_64"));
    EXPECT(pc.shows(
        "No real file systems were harmed. Type 'help', or read README."));
    EXPECT(pc.shows("Try: fire, gradient, neofetch, about"));
}

TEST_CASE("echo, quoting, variables and tilde") {
    Pc pc;
    pc.run("echo \"a   b\" 'c d' $HOME ~ \\$X");
    EXPECT(pc.shows("a   b c d /home/guest /home/guest $X"));
    pc.run("X=5; echo $X '$X' ${X}0");
    EXPECT(pc.shows("5 $X 50"));
    pc.run("echo a\"b\"'c'");
    EXPECT(pc.shows("abc"));
    pc.run("echo $UNSET end");
    EXPECT(pc.shows("end"));
    pc.run("echo -n x; echo y");
    EXPECT(pc.shows("xy"));
}

TEST_CASE("Pipes, lists and exit status") {
    Pc pc;
    pc.run("echo a b c | wc -w");
    EXPECT(pc.shows("      3"));
    pc.run("false || echo second");
    EXPECT(pc.shows("second"));
    pc.run("true && echo both");
    EXPECT(pc.shows("both"));
    pc.run("false && echo no; echo after");
    EXPECT(!pc.shows("\nno\n"));
    EXPECT(pc.shows("after"));
    pc.run("false; echo $?");
    EXPECT(pc.shows("\n1\n"));
    pc.run("seq 1 5 | sort -r | head -n 2");
    EXPECT(pc.shows("5\n4\n"));
}

TEST_CASE("Unknown commands and syntax errors") {
    Pc pc;
    pc.run("frobnicate now");
    EXPECT(pc.shows("matterm: frobnicate: command not found"));
    pc.run("echo $?");
    EXPECT(pc.shows("127"));
    pc.run("echo \"open");
    EXPECT(pc.shows("unexpected EOF"));
    pc.run("| wc");
    EXPECT(pc.shows("syntax error near unexpected token `|'"));
    pc.run("echo hi >");
    EXPECT(pc.shows("syntax error near unexpected token `newline'"));
    pc.run("/nope/x");
    EXPECT(pc.shows("/nope/x: No such file or directory"));
    pc.run("/home");
    EXPECT(pc.shows("/home: Is a directory"));
}

TEST_CASE("ls shows visible files, -a all, -l details") {
    Pc pc;
    pc.run("ls");
    EXPECT(pc.shows("README"));
    EXPECT(pc.shows("projects"));
    EXPECT(!pc.shows(".plan"));
    pc.run("clear");
    pc.run("ls -a");
    EXPECT(pc.shows(".plan"));
    EXPECT(pc.shows(".bashrc"));
    pc.run("clear");
    pc.run("ls -l");
    EXPECT(pc.shows("total"));
    EXPECT(pc.shows("-rw-r--r-- 1 guest guest"));
    EXPECT(pc.shows("drwxr-xr-x 3 guest guest"));
    pc.run("ls /nope");
    EXPECT(pc.shows("ls: cannot access '/nope': No such file or directory"));
    EXPECT(pc.parser.modes().applicationCursor == false);
}

TEST_CASE("cd, pwd and the prompt follow the directory") {
    Pc pc;
    pc.run("cd projects");
    EXPECT(pc.last() == "guest@matterm:~/projects$");
    pc.run("pwd");
    EXPECT(pc.shows("/home/guest/projects"));
    pc.run("cd ../..");
    EXPECT(pc.last() == "guest@matterm:/home$");
    pc.run("cd -");
    EXPECT(pc.last() == "guest@matterm:~/projects$");
    pc.run("cd");
    EXPECT(pc.last() == kPrompt);
    pc.run("cd /nope");
    EXPECT(pc.shows("cd: /nope: No such file or directory"));
    pc.run("cd README");
    EXPECT(pc.shows("cd: README: Not a directory"));
}

TEST_CASE(
    "A long path is abbreviated in the prompt so the line keeps its room") {
    Pc pc(false, 50, 10);
    pc.run("mkdir -p aaa/bbb/ccc/ddd");
    pc.run("cd aaa/bbb/ccc/ddd");
    EXPECT(pc.last() == "guest@matterm:.../ccc/ddd$");
    pc.run("pwd");
    EXPECT(pc.shows("/home/guest/aaa/bbb/ccc/ddd"));
    pc.run("cd");
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Redirects create, append and read files") {
    Pc pc;
    pc.run("echo hi > f");
    pc.run("echo more >> f");
    pc.run("cat f");
    EXPECT(pc.shows("hi\nmore"));
    pc.run("wc -l < f");
    EXPECT(pc.shows("      2"));
    pc.run("cat missing");
    EXPECT(pc.shows("cat: missing: No such file or directory"));
    pc.run("echo $?");
    EXPECT(pc.shows("\n1\n"));
    pc.run("echo x > /etc/motd");
    EXPECT(pc.shows("/etc/motd: Permission denied"));
    pc.run("cat /etc/shadow");
    EXPECT(pc.shows("cat: /etc/shadow: Permission denied"));
    pc.run("cat < /nope");
    EXPECT(pc.shows("/nope: No such file or directory"));
}

TEST_CASE("Wildcards expand against the directory") {
    Pc pc;
    pc.run("touch a.txt b.txt c.log");
    pc.run("echo *.txt");
    EXPECT(pc.shows("a.txt b.txt"));
    pc.run("echo ?.log \"*.txt\" *.zzz");
    EXPECT(pc.shows("c.log *.txt *.zzz"));
    pc.run("rm *.txt");
    pc.run("clear");
    pc.run("ls");
    EXPECT(!pc.shows("a.txt"));
    EXPECT(pc.shows("c.log"));
}

TEST_CASE("File commands: mkdir, cp, mv, rm, find, tree") {
    Pc pc;
    pc.run("mkdir -p x/y/z");
    pc.run("echo data > x/f");
    pc.run("cp x/f x/g");
    pc.run("mv x/g x/y/h");
    pc.run("find x");
    EXPECT(pc.shows("x/y/h"));
    EXPECT(pc.shows("x/y/z"));
    pc.run("clear");
    pc.run("tree x");
    EXPECT(pc.shows("|-- f"));
    EXPECT(pc.shows("`-- y"));
    pc.run("clear");
    pc.run("rm x");
    EXPECT(pc.shows("rm: cannot remove 'x': Is a directory"));
    pc.run("rm -r x");
    pc.run("ls x");
    EXPECT(pc.shows("cannot access 'x'"));
    pc.run("rm /etc/passwd");
    EXPECT(pc.shows("rm: cannot remove '/etc/passwd': Permission denied"));
    pc.run("rm -rf /");
    EXPECT(pc.shows("it is dangerous to operate recursively on '/'"));
    pc.run("ls /etc");
    EXPECT(pc.shows("passwd"));
}

TEST_CASE("Text tools: grep, head, tail, sort, uniq, wc") {
    Pc pc;
    pc.run("grep -n ember /usr/share/dict/words");
    EXPECT(pc.shows("7:ember"));
    pc.run("grep -c e /usr/share/dict/words");
    EXPECT(pc.shows("\n1"));
    pc.run("clear");
    pc.run("seq 1 20 | tail -n 3");
    EXPECT(pc.shows("18\n19\n20"));
    pc.run("printf");
    pc.run("echo b a b | wc -w");
    pc.run("clear");
    pc.run("echo x > t; echo x >> t; echo y >> t; sort t | uniq -c");
    EXPECT(pc.shows("      2 x"));
    EXPECT(pc.shows("      1 y"));
    pc.run("grep -i EMBER /usr/share/dict/words | wc -l");
    EXPECT(pc.shows("      1"));
}

TEST_CASE("System commands") {
    Pc pc;
    pc.run("whoami; hostname; uname -s");
    EXPECT(pc.shows("guest\nmatterm\nLinux"));
    pc.run("id");
    EXPECT(pc.shows("uid=1000(guest)"));
    pc.run("date +%F");
    EXPECT(pc.shows("2026-01-01"));
    pc.run("which ls fire");
    EXPECT(pc.shows("/bin/ls"));
    pc.run("echo $PATH");
    EXPECT(pc.shows("/usr/local/bin:/usr/bin:/bin"));
    pc.run("yes | head -n 2");
    EXPECT(pc.shows("y\ny"));
}

TEST_CASE("History, cursor editing and control keys") {
    Pc pc;
    pc.run("echo one");
    pc.type("\x1b[A"); // Up recalls it.
    EXPECT(pc.last() == kPrompt + " echo one");
    pc.type("\x1b[D\x1b[D\x1b[D"); // Left three times.
    pc.type("X");
    EXPECT(pc.last() == kPrompt + " echo Xone");
    pc.type("\x7f"); // Backspace removes the X.
    pc.type("\x1b[B");
    EXPECT(pc.last() == kPrompt);
    pc.type("abc");
    pc.type("\x15"); // Ctrl-U.
    EXPECT(pc.last() == kPrompt);
    pc.type("abc\x03"); // Ctrl-C.
    EXPECT(pc.shows("abc^C"));
    EXPECT(pc.last() == kPrompt);
    pc.type("echo end\x01X\x05!\r"); // Home, X, End, !.
    EXPECT(pc.shows("matterm: Xecho: command not found"));
}

TEST_CASE("The line stays on one row, extra input is refused") {
    Pc pc;
    pc.type(std::string(100, 'x'));
    std::size_t rowsWithPrompt = 0;
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        if (pc.row(y).find(kPrompt) == 0) {
            ++rowsWithPrompt;
        }
    }
    EXPECT_EQ(rowsWithPrompt, std::size_t(1));
    EXPECT(pc.row(pc.model.cursor().row).size() <= 79);
}

TEST_CASE("Tab completes commands and paths, a second Tab lists choices") {
    Pc pc;
    pc.type("ech\t");
    EXPECT(pc.last() == kPrompt + " echo");
    pc.type("\x15"
            "cat REA\t");
    EXPECT(pc.last() == kPrompt + " cat README");
    pc.type("\x15"
            "cd pro\t");
    EXPECT(pc.last() == kPrompt + " cd projects/");
    pc.type("\x15"
            "cat /usr/s\t");
    EXPECT(pc.last() == kPrompt + " cat /usr/share/");
    pc.type("\x15"
            "c\t\t");
    EXPECT(pc.shows("cat"));
    EXPECT(pc.shows("cp"));
    EXPECT(pc.shows("cd"));
    pc.type("\x15");
    pc.type("wh\t"); // Unique: whoami and which share "wh", so only the prefix.
    EXPECT(pc.last() == kPrompt + " wh");
}

TEST_CASE("exit and Ctrl-D end the session") {
    Pc a;
    a.run("exit 3");
    EXPECT(a.exitCode.has_value() && *a.exitCode == 3);
    Pc b;
    b.type("\x04");
    EXPECT(b.exitCode.has_value() && *b.exitCode == 0);
    Pc c;
    c.run("false; exit");
    EXPECT(c.exitCode.has_value() && *c.exitCode == 1);
    Pc d;
    d.run("exit abc");
    EXPECT(d.exitCode.has_value() && *d.exitCode == 2);
}

TEST_CASE("reset restores the first state") {
    Pc pc;
    pc.run("rm -r projects");
    pc.run("echo x > scratch");
    pc.run("reset");
    EXPECT(pc.shows("restored to its first state"));
    EXPECT(pc.sys.world().history.empty());
    pc.run("ls");
    EXPECT(pc.shows("projects"));
    EXPECT(!pc.shows("scratch"));
}

TEST_CASE("Mouse reports and other replies are ignored at the prompt") {
    Pc pc;
    pc.type("\x1b[<0;5;5M\x1b[<0;5;5m\x1b[12;40R");
    EXPECT(pc.last() == kPrompt);
}

TEST_SUIT_END
