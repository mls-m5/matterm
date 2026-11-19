#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t received = 0;
void signalReceived(int value) {
    received = value;
}
void ready() {
    std::puts("READY");
    std::fflush(stdout);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        return 90;
    }
    if (std::strcmp(argv[1], "tty") == 0) {
        winsize size{};
        if (!isatty(0) || !isatty(1) || !isatty(2) ||
            tcgetpgrp(0) != getpgrp() || ioctl(0, TIOCGWINSZ, &size) < 0) {
            return 91;
        }
        std::printf(
            "TTY:%u:%u:%s\n", size.ws_col, size.ws_row, std::getenv("TERM"));
        return 7;
    }
    if (std::strcmp(argv[1], "echo") == 0) {
        termios settings{};
        if (tcgetattr(0, &settings) < 0) {
            return 92;
        }
        settings.c_lflag &= ~ECHO;
        if (tcsetattr(0, TCSANOW, &settings) < 0) {
            return 93;
        }
        ready();
        char input[128]{};
        const auto size = read(0, input, sizeof(input) - 1);
        if (size <= 0) {
            return 94;
        }
        std::printf("READ:%s", input);
        return 0;
    }
    if (std::strcmp(argv[1], "resize") == 0 ||
        std::strcmp(argv[1], "signal") == 0) {
        const int signal =
            std::strcmp(argv[1], "resize") == 0 ? SIGWINCH : SIGINT;
        struct sigaction action {};
        action.sa_handler = signalReceived;
        sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr);
        ready();
        while (!received) {
            usleep(1000);
        }
        if (signal == SIGWINCH) {
            winsize size{};
            if (ioctl(0, TIOCGWINSZ, &size) < 0) {
                return 95;
            }
            std::printf("SIZE:%u:%u\n", size.ws_col, size.ws_row);
        }
        else {
            std::puts("SIGINT");
        }
        return 0;
    }
    if (std::strcmp(argv[1], "vt") == 0) {
        termios settings{};
        if (tcgetattr(0, &settings) < 0) {
            return 97;
        }
        cfmakeraw(&settings);
        if (tcsetattr(0, TCSANOW, &settings) < 0) {
            return 98;
        }
        std::printf("\x1b[2J\x1b[H\x1b[32mmatterm PTY\x1b[0m\r\n\x1b[6n");
        std::fflush(stdout);
        char response[64]{};
        std::size_t size = 0;
        while (size + 1 < sizeof(response)) {
            const auto count = read(0, response + size, 1);
            if (count <= 0) {
                return 99;
            }
            if (response[size++] == 'R') {
                break;
            }
        }
        if (std::strcmp(response, "\x1b[2;1R") != 0) {
            return 100;
        }
        std::printf("REPLY_OK\r\nDONE");
        return 0;
    }
    if (std::strcmp(argv[1], "flood") == 0) {
        char bytes[4096];
        std::memset(bytes, 'x', sizeof(bytes));
        for (int i = 0; i < 32; ++i) {
            std::size_t sent = 0;
            while (sent < sizeof(bytes)) {
                const auto count = write(1, bytes + sent, sizeof(bytes) - sent);
                if (count <= 0) {
                    return 101;
                }
                sent += std::size_t(count);
            }
        }
        std::printf("\r\nEND");
        return 0;
    }
    if (std::strcmp(argv[1], "hold") == 0) {
        ready();
        while (true) {
            pause();
        }
    }
    return 96;
}
