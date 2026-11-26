#include "mls-unit-test/unittest.h"
#include "serverwindow.h"

#include <sys/wait.h>

TEST_SUIT_BEGIN(ServerWindow)
TEST_CASE("New SSH window uses a separate child and rejects unsafe hosts") {
    pid_t child = -1;
    std::string error;
    EXPECT(!frontend::spawnServerWindow(
        "/bin/true", "-oProxyCommand=bad", child, error));
    EXPECT_EQ(child, pid_t(-1));
    EXPECT(!frontend::spawnServerWindow(
        "/does/not/exist", "server", child, error));
    EXPECT(
        frontend::spawnServerWindow("/bin/true", "user@server", child, error));
    int status = -1;
    EXPECT(::waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
TEST_SUIT_END
