#include "mls-unit-test/unittest.h"
#include "serverwindow.h"

#include <sys/wait.h>

TEST_SUIT_BEGIN(ServerWindow)
TEST_CASE(
    "New SSH window starts independently and reports EOF without readiness") {
    frontend::ServerWindow window;
    std::string error;
    EXPECT(!frontend::spawnServerWindow(
        "/bin/true", "-oProxyCommand=bad", window, error));
    EXPECT_EQ(window.pid, pid_t(-1));
    EXPECT(!frontend::spawnServerWindow(
        "/does/not/exist", "server", window, error));
    EXPECT(
        frontend::spawnServerWindow("/bin/true", "user@server", window, error));
    int status = -1;
    EXPECT(::waitpid(window.pid, &status, 0) == window.pid);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char byte;
    EXPECT(::read(window.statusFd, &byte, 1) == 0);
    ::close(window.statusFd);
}
TEST_CASE("Startup messages are bounded to one line and ignore closed peers") {
    int ends[2];
    EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    frontend::sendServerStatus(ends[0], 'E', "connection\nfailed");
    char data[128]{};
    const auto count = ::read(ends[1], data, sizeof(data));
    EXPECT(std::string(data, std::size_t(count)) == "E connection failed\n");
    ::close(ends[1]);
    frontend::sendServerStatus(ends[0],
                               'R'); // Must not SIGPIPE on parent exit.
    ::close(ends[0]);
}
TEST_CASE("Remote copy requires explicit GUI approval; disconnect denies it") {
    int ends[2];
    EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ends[1]);
        _exit(
            frontend::requestServerConfirmation(ends[0], "Copy remote daemon?")
                ? 0
                : 1);
    }
    EXPECT(child > 0);
    ::close(ends[0]);
    char question[128]{};
    const auto count = ::read(ends[1], question, sizeof(question));
    EXPECT(std::string(question, std::size_t(count)) ==
           "I Copy remote daemon?\n");
    EXPECT(frontend::answerServerConfirmation(ends[1], true));
    int status = -1;
    EXPECT(::waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ::close(ends[1]);

    EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    const pid_t denied = ::fork();
    if (denied == 0) {
        ::close(ends[1]);
        _exit(
            frontend::requestServerConfirmation(ends[0], "Copy remote daemon?")
                ? 1
                : 0);
    }
    EXPECT(denied > 0);
    ::close(ends[0]);
    EXPECT(::read(ends[1], question, sizeof(question)) > 0);
    ::close(ends[1]); // Parent window closed without approving.
    EXPECT(::waitpid(denied, &status, 0) == denied);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
TEST_SUIT_END
