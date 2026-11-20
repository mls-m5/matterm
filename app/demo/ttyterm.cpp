#include "ttyterm.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ttyterm {
namespace {
bool contains(const char *haystack, const char *needle) {
    return haystack && std::strstr(haystack, needle);
}
} // namespace

Caps detect() {
    Caps caps;
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

bool RawMode::enter() {
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

RawMode::~RawMode() {
    if (active) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
    }
}

void size(int &columns, int &rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        columns = ws.ws_col;
        rows = ws.ws_row;
    }
}

} // namespace ttyterm
