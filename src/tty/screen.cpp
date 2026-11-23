#include "tty/screen.h"

namespace tty {
namespace {
int shapeCode(const Cursor &cursor) {
    const int base = cursor.shape == Cursor::Shape::Block       ? 1
                     : cursor.shape == Cursor::Shape::Underline ? 3
                                                                : 5;
    return base + (cursor.blink ? 0 : 1);
}

// The bytes that take the terminal from `from` to `to`.
std::string optionBytes(const ScreenOptions &from, const ScreenOptions &to) {
    std::string out;
    if (from.mouse != to.mouse) {
        const auto code = [](ScreenOptions::Mouse m) {
            return m == ScreenOptions::Mouse::Click  ? "1000"
                   : m == ScreenOptions::Mouse::Drag ? "1002"
                                                     : "1003";
        };
        if (from.mouse != ScreenOptions::Mouse::Off) {
            out += std::string("\x1b[?") + code(from.mouse) + "l";
            if (to.mouse == ScreenOptions::Mouse::Off) {
                out += "\x1b[?1006l";
            }
        }
        if (to.mouse != ScreenOptions::Mouse::Off) {
            out += std::string("\x1b[?") + code(to.mouse) + "h";
            if (from.mouse == ScreenOptions::Mouse::Off) {
                out += "\x1b[?1006h";
            }
        }
    }
    if (from.bracketedPaste != to.bracketedPaste) {
        out += to.bracketedPaste ? "\x1b[?2004h" : "\x1b[?2004l";
    }
    if (from.focusEvents != to.focusEvents) {
        out += to.focusEvents ? "\x1b[?1004h" : "\x1b[?1004l";
    }
    return out;
}
} // namespace

Screen::Screen(int columns, int rows, Caps caps, ScreenOptions options)
    : canvas_(columns, rows, caps)
    , caps_(caps)
    , options_(options) {
    canvas_.setWideChars(true);
    canvas_.setAsciiOnly(!caps.unicode);
}

std::string Screen::enter() {
    if (entered_) {
        return {};
    }
    entered_ = true;
    std::string out;
    if (caps_.alternateScreen) {
        out += "\x1b[?1049h";
    }
    out += "\x1b[?25l\x1b[0m\x1b[2J";
    out += optionBytes(ScreenOptions{}, options_);
    hostCursorVisible_ = false;
    cursorKnown_ = false;
    pendingClear_ = false;
    canvas_.invalidate();
    return out;
}

std::string Screen::restoreSequence() const {
    // Every report off, whatever options_ is now: the caller may hold this from
    // before.
    std::string out =
        "\x1b[?1004l\x1b[?2004l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
    out += "\x1b[0 q\x1b[0m\x1b[?25h";
    if (caps_.alternateScreen) {
        out += "\x1b[?1049l";
    }
    else {
        out += "\x1b[" + std::to_string(rows()) + ";1H\r\n";
    }
    return out;
}

std::string Screen::setOptions(const ScreenOptions &options) {
    const ScreenOptions before = options_;
    options_ = options;
    return entered_ ? optionBytes(before, options) : std::string();
}

std::string Screen::leave() {
    if (!entered_) {
        return {};
    }
    entered_ = false;
    return restoreSequence();
}

void Screen::resize(int columns, int rows) {
    canvas_.resize(columns, rows);
    pendingClear_ = true;
    cursorKnown_ = false;
}

void Screen::redraw() {
    canvas_.invalidate();
    pendingClear_ = true;
    cursorKnown_ = false;
}

std::string Screen::present() {
    std::string cells;
    if (pendingClear_) {
        cells = "\x1b[2J";
        pendingClear_ = false;
    }
    cells += canvas_.flush();
    const bool drew = !cells.empty();
    const bool shapeChanged =
        cursor_.visible && (!cursorKnown_ || cursor_.shape != shown_.shape ||
                            cursor_.blink != shown_.blink);
    const bool moved = cursor_.visible && (drew || !cursorKnown_ ||
                                           cursor_.column != shown_.column ||
                                           cursor_.row != shown_.row);
    const bool visibilityChanged = cursor_.visible != hostCursorVisible_;
    if (!drew && !shapeChanged && !moved && !visibilityChanged) {
        return {};
    }
    std::string out;
    if (drew) {
        out += "\x1b[?2026h"; // Synchronized update, ignored where unknown.
        if (hostCursorVisible_) {
            out += "\x1b[?25l";
        }
        out += cells;
    }
    if (shapeChanged) {
        out += "\x1b[" + std::to_string(shapeCode(cursor_)) + " q";
    }
    if (moved) {
        out += "\x1b[" + std::to_string(cursor_.row + 1) + ";" +
               std::to_string(cursor_.column + 1) + "H";
    }
    if (cursor_.visible) {
        out += "\x1b[?25h";
    }
    else if (hostCursorVisible_ && !drew) {
        out += "\x1b[?25l";
    }
    if (drew) {
        out += "\x1b[?2026l";
    }
    hostCursorVisible_ = cursor_.visible;
    shown_ = cursor_;
    cursorKnown_ = true;
    return out;
}

} // namespace tty
