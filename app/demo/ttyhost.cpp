#include "ttyhost.h"

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

bool contains(const char *haystack, const char *needle) {
    return haystack && std::strstr(haystack, needle);
}

// What the terminal in front of us can do, from the environment: the usual
// COLORTERM/TERM/locale conventions.
fakeos::Capabilities detect() {
    fakeos::Capabilities caps;
    const char *colorterm = std::getenv("COLORTERM");
    const char *term = std::getenv("TERM");
    caps.truecolor =
        contains(colorterm, "truecolor") || contains(colorterm, "24bit");
    caps.colors256 = caps.truecolor || contains(term, "256");
    const bool plain = !term || std::string(term) == "linux" ||
                       std::string(term) == "dumb" || contains(term, "vt100") ||
                       contains(term, "vt220");
    caps.alternateScreen = !plain;
    const char *locale = std::getenv("LC_ALL");
    if (!locale || !*locale)
        locale = std::getenv("LC_CTYPE");
    if (!locale || !*locale)
        locale = std::getenv("LANG");
    caps.unicode = contains(locale, "UTF-8") || contains(locale, "utf8") ||
                   contains(locale, "UTF8");
    return caps;
}

bool writeAll(const std::string &bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n =
            ::write(STDOUT_FILENO, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                pollfd out{STDOUT_FILENO, POLLOUT, 0};
                poll(&out, 1, 100);
                continue;
            }
            return false;
        }
        done += std::size_t(n);
    }
    return true;
}

// Puts the terminal back, whatever happens.
struct RawMode {
    termios saved{};
    bool active = false;
    bool enter() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) {
            return false;
        }
        termios raw = saved;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active = tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) == 0;
        return active;
    }
    ~RawMode() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
        }
    }
};

void size(int &columns, int &rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        columns = ws.ws_col;
        rows = ws.ws_row;
    }
}
} // namespace

int runTtyHost(fakeos::Config config) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "matterm-demo --tty needs a terminal on standard input "
                     "and output\n";
        return 1;
    }
    config.capabilities = detect();
    size(config.columns, config.rows);
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
    RawMode raw;
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
        if (!r.output.empty() && !writeAll(r.output)) {
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
            size(columns, rows);
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
    writeAll(
        "\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?25h" +
        std::string(config.capabilities.alternateScreen && system.inProgram()
                        ? "\x1b[?1049l"
                        : "") +
        "\r\n");
    return exitCode.value_or(0);
}
