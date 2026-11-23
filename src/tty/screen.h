#pragma once

// A whole terminal screen: a Canvas plus what surrounds it on a real terminal
// (alternate screen, cursor, mouse/paste/focus reporting) as plain strings to
// write. It does no I/O and knows nothing of what is drawn.
//
//   Screen screen(columns, rows, detectCaps());
//   write(screen.enter());
//   screen.canvas().text(...); screen.setCursor({true, x, y});
//   write(screen.present());          // the smallest update since last time
//   write(screen.leave());            // on every way out
#include "tty/canvas.h"

#include <string>

namespace tty {

struct Cursor {
    enum class Shape { Block, Underline, Bar };
    bool visible = false;
    int column = 0, row = 0;
    Shape shape = Shape::Block;
    bool blink = true;
    friend bool operator==(const Cursor &a, const Cursor &b) {
        return a.visible == b.visible && a.column == b.column &&
               a.row == b.row && a.shape == b.shape && a.blink == b.blink;
    }
    friend bool operator!=(const Cursor &a, const Cursor &b) {
        return !(a == b);
    }
};

// What to ask the host terminal for while the screen is entered. The matching
// reports arrive as input (tty/keys.h).
struct ScreenOptions {
    bool mouse = false;          // Button and drag reports, SGR encoding.
    bool bracketedPaste = false; // Pasted text arrives between markers.
    bool focusEvents = false;
};

class Screen {
public:
    Screen(int columns, int rows, Caps caps = {}, ScreenOptions options = {});

    int columns() const {
        return canvas_.columns();
    }
    int rows() const {
        return canvas_.rows();
    }
    // Draw here; present() sends the difference.
    Canvas &canvas() {
        return canvas_;
    }
    void setCursor(const Cursor &cursor) {
        cursor_ = cursor;
    }

    // Switches to the alternate screen (when the terminal has one), hides the
    // cursor, clears and turns the reports on. Empty when already entered.
    std::string enter();
    // Undoes enter() and everything present() changed (cursor shape, colours).
    // Empty when not entered. leave() is safe to call on every way out.
    std::string leave();
    bool entered() const {
        return entered_;
    }
    // What leave() writes, whatever the state: for a signal handler or a
    // crash, which must write the restoring bytes without calling anything
    // else of this class.
    std::string restoreSequence() const;

    // The terminal changed size (the host's own screen is cleared and every
    // cell is sent again by the next present()).
    void resize(int columns, int rows);
    // The next present() repaints everything (after a stop/continue, or when
    // something else wrote to the terminal).
    void redraw();

    // The bytes that bring the terminal up to date with the canvas and cursor;
    // empty when nothing changed.
    std::string present();

private:
    Canvas canvas_;
    Caps caps_;
    ScreenOptions options_;
    Cursor cursor_, shown_;
    bool entered_ = false;
    bool pendingClear_ = false;
    bool hostCursorVisible_ = false;
    bool cursorKnown_ = false;
};

} // namespace tty
