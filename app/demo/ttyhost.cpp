#include "ttyhost.h"

#include "tty/posix/terminal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
int gWinchPipe[2] = {-1, -1};
volatile std::sig_atomic_t gStop = 0;

void onWinch(int) {
    const char byte = 'w';
    const auto saved = errno;
    (void)!write(gWinchPipe[1], &byte, 1);
    errno = saved;
}
void onStop(int) {
    gStop = 1;
}

} // namespace

int runTtyHost(fakeos::Config config) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "this program needs a terminal on standard input "
                     "and output\n";
        return 1;
    }
    const tty::Caps caps = tty::detectCaps();
    config.capabilities.truecolor = caps.truecolor;
    config.capabilities.colors256 = caps.colors256;
    config.capabilities.alternateScreen = caps.alternateScreen;
    config.capabilities.unicode = caps.unicode;
    tty::windowSize(STDOUT_FILENO, config.columns, config.rows);
    if (pipe(gWinchPipe) != 0) {
        std::cerr << "pipe: " << std::strerror(errno) << '\n';
        return 1;
    }
    struct sigaction action {};
    action.sa_handler = onWinch;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &action, nullptr);
    action.sa_handler = onStop;
    action.sa_flags = 0; // Interrupt the poll.
    for (const int signal : {SIGINT, SIGTERM, SIGHUP}) {
        sigaction(signal, &action, nullptr);
    }
    tty::RawMode raw(STDIN_FILENO);
    if (!raw.enter()) {
        std::cerr << "cannot put the terminal in raw mode\n";
        return 1;
    }
    fakeos::System system(config);
    const auto start = std::chrono::steady_clock::now();
    auto now = [&] {
        return std::chrono::duration_cast<fakeos::Time>(
            std::chrono::steady_clock::now() - start);
    };
    std::optional<int> exitCode;
    auto handle = [&](const fakeos::Reaction &r) {
        if (!r.output.empty() && !tty::writeAll(STDOUT_FILENO, r.output)) {
            gStop = 1;
        }
        if (r.exitCode) {
            exitCode = r.exitCode;
        }
    };
    handle(system.start());
    handle(system.tick(now()));
    char buffer[4096];
    while (!exitCode && !gStop) {
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {gWinchPipe[0], POLLIN, 0}};
        int timeout = -1;
        if (const auto deadline = system.nextDeadline()) {
            timeout = int(
                std::clamp<long long>((*deadline - now()).count(), 0, 1000));
        }
        const int ready = poll(fds, 2, timeout);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready > 0 && (fds[1].revents & POLLIN)) {
            char drain[16];
            (void)!read(gWinchPipe[0], drain, sizeof drain);
            int columns = config.columns, rows = config.rows;
            tty::windowSize(STDOUT_FILENO, columns, rows);
            system.resize(columns, rows);
        }
        if (ready > 0 && (fds[0].revents & (POLLIN | POLLHUP))) {
            const auto n = read(STDIN_FILENO, buffer, sizeof buffer);
            if (n <= 0 && errno != EINTR) {
                break; // End of input: the terminal went away.
            }
            if (n > 0) {
                handle(system.input(std::string_view(buffer, std::size_t(n))));
            }
        }
        if (!exitCode) {
            handle(system.tick(now()));
        }
    }
    // Leave the screen sane even after a signal or while a program ran.
    tty::writeAll(STDOUT_FILENO,
                  "\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?25h" +
                      std::string(config.capabilities.alternateScreen &&
                                          system.inProgram()
                                      ? "\x1b[?1049l"
                                      : "") +
                      "\r\n");
    return exitCode.value_or(0);
}
