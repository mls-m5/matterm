#include "tty/posix/terminal.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace tty {
namespace {
bool contains(const char *haystack, const char *needle) {
    return haystack && std::strstr(haystack, needle);
}
const char *firstSet(std::initializer_list<const char *> names) {
    for (const char *name : names) {
        const char *value = std::getenv(name);
        if (value && *value) {
            return value;
        }
    }
    return nullptr;
}
} // namespace

Caps capsFrom(const char *colorterm, const char *term, const char *locale) {
    Caps caps;
    caps.truecolor =
        contains(colorterm, "truecolor") || contains(colorterm, "24bit");
    caps.colors256 = caps.truecolor || contains(term, "256");
    const bool plain = !term || std::string(term) == "linux" ||
                       std::string(term) == "dumb" || contains(term, "vt100") ||
                       contains(term, "vt220");
    caps.alternateScreen = !plain;
    caps.unicode = contains(locale, "UTF-8") || contains(locale, "utf8") ||
                   contains(locale, "UTF8");
    return caps;
}

Caps detectCaps() {
    return capsFrom(std::getenv("COLORTERM"),
                    std::getenv("TERM"),
                    firstSet({"LC_ALL", "LC_CTYPE", "LANG"}));
}

bool writeAll(int fd, std::string_view bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                pollfd out{fd, POLLOUT, 0};
                poll(&out, 1, 100);
                continue;
            }
            return false;
        }
        done += std::size_t(n);
    }
    return true;
}

bool RawMode::enter() {
    if (active_) {
        return true;
    }
    if (tcgetattr(fd_, &saved_) != 0) {
        return false;
    }
    termios raw = saved_;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(fd_, TCSADRAIN, &raw) == 0;
    return active_;
}

void RawMode::restore() {
    if (active_) {
        tcsetattr(fd_, TCSADRAIN, &saved_);
        active_ = false;
    }
}

void windowSize(int fd, int &columns, int &rows) {
    winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        columns = ws.ws_col;
        rows = ws.ws_row;
    }
}

} // namespace tty
