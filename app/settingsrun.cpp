#include "settingsrun.h"
#include "settingsui.h"
#include "tty/posix/session.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

int runSettings() {
    using namespace frontend::settings;
    RealFileSystem fs;
    SettingsWatcher watcher(
        fs,
        settingsPath(std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME")),
        [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        });
    SettingsUI ui(watcher);
    tty::SessionOptions options;
    options.screen.bracketedPaste = true;
    tty::Session session(options);
    if (!session.start()) {
        (void)std::fputs("settings: stdin and stdout must be a terminal\n",
                         stderr);
        return 1;
    }
    tty::KeyDecoder keys;
    while (ui.running()) {
        ui.poll();
        ui.draw(session.screen().canvas());
        if (!session.present())
            return 1;
        pollfd fds[] = {{STDIN_FILENO, POLLIN, 0},
                        {session.wakeFd(), POLLIN, 0}};
        const int ready = ::poll(fds, 2, 100);
        if (ready < 0 && errno != EINTR)
            return 1;
        if (session.takeEvents().quit)
            break;
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        if (fds[0].revents & POLLIN) {
            char buffer[4096];
            const auto n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n == 0)
                break;
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                return 1;
            }
            for (const auto &key :
                 keys.feed(std::string_view(buffer, std::size_t(n))))
                ui.handle(key);
        }
    }
    return 0;
}
