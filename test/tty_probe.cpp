// A tiny full-screen program for ttysession_test: draws its size, redraws on
// resize, 'q' quits, 'c' crashes, SIGTERM quits through the session.
#include "tty/keys.h"
#include "tty/posix/session.h"

#include <cerrno>
#include <csignal>
#include <poll.h>
#include <string>
#include <unistd.h>

int main() {
    tty::Session session;
    if (!session.start()) {
        return 2;
    }
    tty::KeyDecoder decoder;
    int exitCode = 0;
    for (bool running = true; running;) {
        auto &canvas = session.screen().canvas();
        canvas.clear();
        canvas.text(0,
                    0,
                    "size " + std::to_string(session.screen().columns()) + "x" +
                        std::to_string(session.screen().rows()),
                    {255, 255, 255});
        session.screen().setCursor({true, 0, 1});
        if (!session.present()) {
            return 4;
        }
        pollfd fds[2] = {{0, POLLIN, 0}, {session.wakeFd(), POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) {
            return 5;
        }
        if (fds[0].revents & POLLIN) {
            char buffer[256];
            const auto n = read(0, buffer, sizeof buffer);
            if (n <= 0) {
                return 6;
            }
            for (const auto &key :
                 decoder.feed(std::string(buffer, std::size_t(n)))) {
                if (key.isText("q")) {
                    running = false;
                }
                else if (key.isText("c")) {
                    (void)raise(SIGSEGV);
                }
                else if (key.isText("a")) {
                    std::abort();
                }
            }
        }
        const auto events = session.takeEvents();
        if (events.quit) {
            exitCode = 3;
            running = false;
        }
    }
    return exitCode;
}
