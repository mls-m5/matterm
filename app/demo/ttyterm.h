#pragma once

#include <string>
#include <termios.h>

// What the programs that run in the terminal they were started from (the demo's
// --tty, mattrix, mattrix-chat) have in common: what the terminal can do (from
// the environment), a restoring raw mode, the window size and a robust write.
namespace ttyterm {

struct Caps {
    bool truecolor = false;
    bool colors256 = false;
    bool alternateScreen = true;
    bool unicode = false;
};

// The usual COLORTERM/TERM/locale conventions.
Caps detect();

// All of `bytes` to standard output (waits when the terminal is busy).
bool writeAll(const std::string &bytes);

// Puts the terminal back, whatever happens.
struct RawMode {
    termios saved{};
    bool active = false;
    bool enter();
    ~RawMode();
};

// Leaves the arguments alone when the size is unknown.
void size(int &columns, int &rows);

} // namespace ttyterm
