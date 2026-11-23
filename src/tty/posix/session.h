#pragma once

// A full-screen session on the terminal this process was started from: raw
// mode, the alternate screen, the window size, and the way back. The terminal
// is put back on every way out: normal return, exit(), a stop or termination
// signal, a crash (SIGSEGV and friends) and ^Z / SIGCONT.
//
//   tty::Session session;
//   if (!session.start()) return 1;            // not a terminal
//   for (;;) {
//       draw into session.screen().canvas(); session.present();
//       poll(stdin, session.wakeFd());
//       const auto events = session.takeEvents();
//       if (events.quit) break;                  // resized/resumed: just draw
//   }
//
// Only one Session can be started at a time in a process (the signal handlers
// are process-wide). The handlers write only the restoring bytes and call
// async-signal-safe functions.
#include "tty/posix/terminal.h"
#include "tty/screen.h"

#include <memory>
#include <string>

namespace tty {

struct SessionOptions {
    int inputFd = 0;
    int outputFd = 1;
    ScreenOptions screen;
    Caps caps; // Overrides detection when `detect` is false.
    bool detect = true;
};

class Session {
public:
    explicit Session(SessionOptions options = {});
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Raw mode, alternate screen, signal handlers. False (nothing changed)
    // when the descriptors are not a terminal or another Session is running.
    bool start();
    // Puts everything back; also done by the destructor. Safe to call twice.
    void stop();
    bool running() const {
        return running_;
    }

    // Created by start(), sized to the terminal.
    Screen &screen() {
        return *screen_;
    }
    // Writes what changed to the terminal. False when the write failed (the
    // terminal is gone).
    bool present();

    // Readable when takeEvents() has something. Poll it with the input.
    int wakeFd() const;
    struct Events {
        bool resized = false; // The screen was resized; draw everything again.
        bool resumed =
            false;         // Continued after a stop; the screen is repainted.
        bool quit = false; // SIGINT, SIGTERM or SIGHUP arrived.
    };
    Events takeEvents();

private:
    SessionOptions options_;
    std::unique_ptr<Screen> screen_;
    bool running_ = false;
};

} // namespace tty
