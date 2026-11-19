#pragma once

// Terminal input modes negotiated by the output stream, not renderer state.
struct TerminalModes {
    bool applicationCursor = false;
    bool bracketedPaste = false;
};
