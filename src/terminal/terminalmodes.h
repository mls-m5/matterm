#pragma once

// Terminal input modes negotiated by the output stream, not renderer state.
struct TerminalModes {
    // Which mouse events the application asked to receive (DECSET 9, 1000,
    // 1002, 1003). Off means the terminal keeps the mouse for itself.
    enum class MouseTracking { Off, Press, Normal, Drag, Any };
    // How reports are encoded (DECSET 1005, 1006, 1015). X10 is the default.
    enum class MouseEncoding { X10, Utf8, Sgr, Urxvt };

    bool applicationCursor = false;
    bool applicationKeypad = false;
    bool bracketedPaste = false;
    bool focusReporting = false;
    MouseTracking mouseTracking = MouseTracking::Off;
    MouseEncoding mouseEncoding = MouseEncoding::X10;
};
