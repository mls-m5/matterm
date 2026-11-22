// matterm-daemon: keeps terminal sessions alive on a private Unix socket (docs/tasks/daemon.md).
#include "daemon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void usage() {
    std::fputs("usage: matterm-daemon [--daemon-socket PATH] [--idle-seconds N] [--daemonize]\n"
               "  --daemon-socket PATH  use PATH instead of $XDG_RUNTIME_DIR/matterm/daemon.sock\n"
               "  --idle-seconds N      exit after N seconds with no sessions and no client (default 60, 0 = never)\n"
               "  --daemonize           detach; the parent returns once the socket is ready\n",
               stderr);
}
} // namespace

int main(int argc, char **argv) {
    auto config = mtd::defaultConfig();
    bool detach = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--daemon-socket" && i + 1 < argc) {
            config.socketPath = argv[++i];
            config.privateDirectory = false;
        }
        else if (a == "--idle-seconds" && i + 1 < argc) {
            char *end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (!end || *end || v < 0 || v > 86400) {
                usage();
                return 2;
            }
            config.idleExitSeconds = unsigned(v);
        }
        else if (a == "--daemonize") {
            detach = true;
        }
        else {
            usage();
            return a == "--help" ? 0 : 2;
        }
    }
    int ready[2] = {-1, -1};
    if (detach) {
        if (::pipe(ready) != 0) {
            std::perror("pipe");
            return 1;
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::perror("fork");
            return 1;
        }
        if (pid > 0) {
            // Parent: wait for the child to report; relay its error text.
            ::close(ready[1]);
            char buf[512];
            const auto n = ::read(ready[0], buf, sizeof buf - 1);
            if (n == 1 && buf[0] == 0) return 0;
            if (n > 1) std::fprintf(stderr, "matterm-daemon: %.*s\n", int(n - 1), buf + 1);
            return 1;
        }
        ::close(ready[0]);
        ::setsid();
    }
    mtd::Daemon d(std::move(config));
    std::string error;
    if (!d.start(error)) {
        if (detach) {
            const std::string msg = '\1' + error;
            [[maybe_unused]] const auto w = ::write(ready[1], msg.data(), msg.size());
        }
        else {
            std::fprintf(stderr, "matterm-daemon: %s\n", error.c_str());
        }
        return 1;
    }
    if (detach) {
        const char ok = 0;
        [[maybe_unused]] const auto w = ::write(ready[1], &ok, 1);
        ::close(ready[1]);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 1);
            ::dup2(null, 2);
            if (null > 2) ::close(null);
        }
    }
    d.run();
    return 0;
}
