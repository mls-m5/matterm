#pragma once

// matmux passthrough (docs/tasks/matmux-passthrough.md): what to write to the host terminal when a client
// attaches to a terminal whose program is already running, so that the host looks as if the program's
// output had been passing through all along; and how to put the host terminal back.
//
//   TerminalModel model(cols, rows, scrollback);  VtParser parser(model);
//   parser.feed(replay);                          // the daemon's replay, nothing drawn
//   write(snapshot(model, parser.modes()));       // once, on attach
//   ... then every byte from the daemon goes to the host unchanged ...
//   write(hostRestore(model.alternateScreen()));  // on every way out
//
// The snapshot is: the model's scrollback as ordinary lines (they land in the host's scrollback), then the
// screen as exactly `rows` lines, so that the visible area equals the model's screen with the same row
// alignment whatever the host showed before; then the scroll region, origin mode, cursor (position, shape,
// visibility) and the input modes the program asked for (application cursor and keypad, bracketed paste,
// focus reports, mouse tracking and encoding). A program in the alternate screen: the host enters it and
// the screen is drawn there (no scrollback, there is none).
//
// Known limits (not hidden): the model does not know the current pen (the SGR in force for the next
// character), tab stops, character sets, autowrap mode or a pending wrap at the last column; after the
// snapshot these are the host's defaults until the program sets them again. The model has no wide
// characters (one code point per cell), so a row with them is written as the model holds it. When the
// program is in the alternate screen, the primary screen behind it is not restored: after the program
// leaves, the host shows what it showed before the attach.
//
// The terminal's width and height must be the host's: size the model to the host before feeding it.
#include "terminalmodel.h"
#include "terminalmodes.h"

#include <cstddef>
#include <string>

namespace tty {

struct SnapshotOptions {
    bool scrollback = true;                    // Write the saved lines (false: only the screen).
    std::size_t maxScrollbackRows = 10000;     // The newest this many.
};

// The bytes that make a host terminal of the model's size show the model's state (see above).
std::string snapshot(const TerminalModel &model, const TerminalModes &modes, const SnapshotOptions &options = {});

// Puts the host terminal back after passthrough, whatever the program did: leaves the alternate screen when
// `inAlternateScreen`, turns off every input report (mouse in all its forms, bracketed paste, focus,
// application cursor and keypad), resets scroll region, origin mode, character set, SGR and cursor shape,
// shows the cursor, and ends with a newline so that the next output starts on a fresh line.
std::string hostRestore(bool inAlternateScreen);

} // namespace tty
