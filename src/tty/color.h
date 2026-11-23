#pragma once

// Colour vocabulary of the tty layer: an RGB value, what the terminal can show,
// and the reduction of a colour to what it can show.
#include <cstdint>
#include <string>

namespace tty {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    friend bool operator==(Rgb a, Rgb b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    friend bool operator!=(Rgb a, Rgb b) {
        return !(a == b);
    }
};

// What the terminal in front of the program supports. The defaults are "all of
// it"; detectCaps() (tty/posix/terminal.h) narrows them from the environment.
struct Caps {
    bool truecolor = true;
    bool colors256 = true;
    bool alternateScreen = true;
    bool unicode = true;
};

// The closest entry of the xterm 256-colour palette (16..255: the 6x6x6 cube
// and the grays) and of the 16 ANSI colours (0..15).
int nearestXterm256(Rgb color);
int nearestAnsi16(Rgb color);

// Appends the SGR parameters that select `color` as foreground or background
// ("38;2;r;g;b", "48;5;n", "91", ...), reduced to what `caps` allows.
void appendSgrColor(std::string &out, bool foreground, Rgb color, Caps caps);

// The same for a cell's colour: `isDefault` selects the terminal's own colour
// (39/49), a palette `index` >= 0 passes through (30..37, 90..97, 38;5;n), else
// the RGB value is used.
void appendSgrColor(std::string &out,
                    bool foreground,
                    Rgb color,
                    bool isDefault,
                    int index,
                    Caps caps);

} // namespace tty
