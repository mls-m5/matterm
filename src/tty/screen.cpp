#include "tty/screen.h"

namespace tty {
namespace {
int shapeCode(const Cursor &cursor) {
    const int base = cursor.shape == Cursor::Shape::Block       ? 1
                     : cursor.shape == Cursor::Shape::Underline ? 3
                                                                : 5;
    return base + (cursor.blink ? 0 : 1);
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
    if (options_.mouse) {
        out += "\x1b[?1002h\x1b[?1006h";
    }
    if (options_.bracketedPaste) {
        out += "\x1b[?2004h";
    }
    if (options_.focusEvents) {
        out += "\x1b[?1004h";
    }
    hostCursorVisible_ = false;
    cursorKnown_ = false;
    pendingClear_ = false;
    canvas_.invalidate();
    return out;
}

std::string Screen::restoreSequence() const {
    std::string out;
    if (options_.focusEvents) {
        out += "\x1b[?1004l";
    }
    if (options_.bracketedPaste) {
        out += "\x1b[?2004l";
    }
    if (options_.mouse) {
        out += "\x1b[?1006l\x1b[?1002l";
    }
    out += "\x1b[0 q\x1b[0m\x1b[?25h";
    if (caps_.alternateScreen) {
        out += "\x1b[?1049l";
    }
    else {
        out += "\x1b[" + std::to_string(rows()) + ";1H\r\n";
    }
    return out;
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
