#include "workload.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t interrupted = 0;
void interrupt(int signal) {
    interrupted = signal;
}

struct TerminalInput {
    termios original{};
    struct sigaction oldInt {
    }, oldTerm{}, oldHup{};
    bool active = false;
    explicit TerminalInput(bool enable) {
        if (!enable) {
            return;
        }
        if (!isatty(0) || !isatty(1) || tcgetattr(0, &original) < 0) {
            throw std::runtime_error(
                "--barrier requires terminal stdin and stdout");
        }
        struct sigaction handler {};
        handler.sa_handler = interrupt;
        sigemptyset(&handler.sa_mask);
        sigaction(SIGINT, &handler, &oldInt);
        sigaction(SIGTERM, &handler, &oldTerm);
        sigaction(SIGHUP, &handler, &oldHup);
        auto settings = original;
        // Keep ISIG so Ctrl+C exits through cleanup. Disable canonical/echo to
        // read cursor reports promptly, without changing output processing.
        settings.c_lflag &= ~(ICANON | ECHO);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &settings) < 0) {
            sigaction(SIGINT, &oldInt, nullptr);
            sigaction(SIGTERM, &oldTerm, nullptr);
            sigaction(SIGHUP, &oldHup, nullptr);
            throw std::runtime_error("Could not set terminal input mode");
        }
        active = true;
    }
    ~TerminalInput() {
        if (active) {
            tcsetattr(0, TCSANOW, &original);
            sigaction(SIGINT, &oldInt, nullptr);
            sigaction(SIGTERM, &oldTerm, nullptr);
            sigaction(SIGHUP, &oldHup, nullptr);
        }
    }
};
void writeAll(std::string_view bytes, std::size_t chunk) {
    while (!bytes.empty()) {
        if (interrupted) {
            throw std::runtime_error("Benchmark interrupted");
        }
        const auto count =
            ::write(1, bytes.data(), std::min(chunk, bytes.size()));
        if (count > 0) {
            bytes.remove_prefix(std::size_t(count));
        }
        else if (count < 0 && errno == EINTR) {
            continue;
        }
        else {
            throw std::runtime_error(std::string("stdout write failed: ") +
                                     std::strerror(errno));
        }
    }
}
void cursorBarrier() {
    writeAll("\x1b[6n", 4);
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    // Bounded CSI row;column R recognizer. Do not accept arbitrary input as
    // ack.
    int state = 0, digits = 0;
    std::size_t consumed = 0;
    while (Clock::now() < deadline && consumed < 4096 && !interrupted) {
        pollfd input{0, POLLIN, 0};
        const int ready = poll(&input, 1, 50);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            break;
        }
        if (!ready) {
            continue;
        }
        char bytes[128];
        const auto count = read(0, bytes, sizeof(bytes));
        if (count <= 0) {
            continue;
        }
        consumed += std::size_t(count);
        for (ssize_t i = 0; i < count; ++i) {
            const char ch = bytes[i];
            if (ch == '\x1b') {
                state = 1;
                digits = 0;
            }
            else if (state == 1 && ch == '[') {
                state = 2;
                digits = 0;
            }
            else if ((state == 2 || state == 3) && ch >= '0' && ch <= '9' &&
                     digits < 6) {
                ++digits;
            }
            else if (state == 2 && digits && ch == ';') {
                state = 3;
                digits = 0;
            }
            else if (state == 3 && digits && ch == 'R') {
                return;
            }
            else {
                state = 0;
            }
        }
    }
    throw std::runtime_error("DSR barrier timed out/invalid (3 seconds); no "
                             "display-completion claim");
}
} // namespace

int main(int argc, char **argv) {
    try {
        const std::string name = argc > 1 ? argv[1] : "long";
        const auto requested =
            argc > 2 ? std::stoull(argv[2]) : 2 * 1024 * 1024;
        const auto chunk = argc > 3 ? std::stoull(argv[3]) : 4096;
        const bool barrier = argc == 5 && std::string(argv[4]) == "--barrier";
        if (argc > 5 || (argc == 5 && !barrier) || chunk == 0) {
            throw std::invalid_argument(
                "Usage: matterm-terminal-bench [long|short|color|redraw] "
                "[bytes] [chunk] [--barrier]");
        }
        const auto bytes = benchmark::workload(name, std::size_t(requested));
        TerminalInput input(barrier);
        if (barrier) {
            cursorBarrier();
        } // Preflight/drain old reports before timing.
        const auto begin = Clock::now();
        writeAll(bytes, std::size_t(chunk));
        const auto written = Clock::now();
        if (barrier) {
            cursorBarrier();
        }
        const auto processed = Clock::now();
        const double writeSeconds =
            std::chrono::duration<double>(written - begin).count();
        const double processSeconds =
            std::chrono::duration<double>(processed - begin).count();
        // stderr, after timing. Run repeatedly and aggregate outside the
        // terminal.
        std::cerr << "\nworkload=" << name << " bytes=" << bytes.size()
                  << " chunk=" << chunk << " write_ms=" << writeSeconds * 1000;
        if (barrier) {
            std::cerr << " dsr_processed_ms=" << processSeconds * 1000;
        }
        else {
            std::cerr << " dsr_processed_ms=not_measured";
        }
        std::cerr << " display_complete=unknown\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << '\n' << error.what() << '\n';
        return interrupted ? 128 + interrupted : 1;
    }
}
