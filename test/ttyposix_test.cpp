#include "mls-unit-test/unittest.h"
#include "tty/posix/terminal.h"

#include <csignal>
#include <unistd.h>

using namespace tty;

TEST_SUIT_BEGIN(TtyTerminal)

TEST_CASE("Capabilities follow COLORTERM, TERM and the locale") {
    auto caps = capsFrom("truecolor", "xterm-256color", "sv_SE.UTF-8");
    EXPECT(caps.truecolor && caps.colors256 && caps.unicode);
    EXPECT(caps.alternateScreen);

    caps = capsFrom("24bit", "xterm", "en_US.utf8");
    EXPECT(caps.truecolor && caps.colors256 && caps.unicode);

    caps = capsFrom(nullptr, "xterm-256color", "C");
    EXPECT(!caps.truecolor && caps.colors256 && !caps.unicode);

    caps = capsFrom(nullptr, "xterm", nullptr);
    EXPECT(!caps.truecolor && !caps.colors256 && !caps.unicode);
}

TEST_CASE("Plain terminals get no alternate screen") {
    for (const char *term : {"linux", "dumb", "vt100", "vt220", "vt220-8bit"}) {
        EXPECT(!capsFrom(nullptr, term, nullptr).alternateScreen);
    }
    EXPECT(!capsFrom(nullptr, nullptr, nullptr).alternateScreen);
    EXPECT(capsFrom(nullptr, "screen", nullptr).alternateScreen);
    EXPECT(capsFrom(nullptr, "tmux-256color", nullptr).alternateScreen);
}

TEST_CASE("writeAll writes to any descriptor, also a full pipe in pieces") {
    int fds[2];
    EXPECT_EQ(pipe(fds), 0);
    EXPECT(writeAll(fds[1], "hello"));
    char buffer[16] = {};
    EXPECT_EQ(read(fds[0], buffer, sizeof buffer), ssize_t(5));
    EXPECT(std::string(buffer, 5) == "hello");
    close(fds[0]);
    // The caller owns signals, not the library.
    EXPECT(std::signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    EXPECT(!writeAll(fds[1], "x")); // Reader gone: reports failure.
    close(fds[1]);
}

TEST_CASE("size and raw mode leave a non-terminal alone") {
    int columns = 7, rows = 9;
    windowSize(-1, columns, rows); // Not a terminal: untouched.
    EXPECT(columns == 7 && rows == 9);
    RawMode raw(-1);
    EXPECT(!raw.enter());
}

TEST_SUIT_END
