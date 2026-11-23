#pragma once

// The POSIX end of the tty layer: what the terminal can do (from the
// environment), a raw mode that puts the terminal back, the window size and a
// robust write. Nothing here knows what is drawn.
#include "tty/color.h"

#include <string_view>
#include <termios.h>

namespace tty {

// The usual COLORTERM/TERM/locale conventions, from the given values (null when
// unset): the testable core of detectCaps().
Caps capsFrom(const char *colorterm, const char *term, const char *locale);
// capsFrom() for this process's environment (COLORTERM, TERM, and the first set
// of LC_ALL, LC_CTYPE, LANG).
Caps detectCaps();

// All of `bytes` to `fd` (waits when it is busy, retries when interrupted).
// False when the write fails; SIGPIPE is the caller's business.
bool writeAll(int fd, std::string_view bytes);

// Puts the terminal back, whatever happens. enter() fails (false) when `fd` is
// not a terminal; the destructor restores only what enter() changed.
class RawMode {
public:
    explicit RawMode(int fd = 0)
        : fd_(fd) {}
    RawMode(const RawMode &) = delete;
    RawMode &operator=(const RawMode &) = delete;
    ~RawMode() {
        restore();
    }
    bool enter();
    void restore();
    bool active() const {
        return active_;
    }

private:
    int fd_;
    termios saved_{};
    bool active_ = false;
};

// The size of the terminal on `fd`; leaves the arguments alone when unknown.
void windowSize(int fd, int &columns, int &rows);

} // namespace tty
