#pragma once

#include "terminalmodes.h"

#include <string>
#include <string_view>

// GUI-independent keyboard/paste encoding. Printable text is already UTF-8.
class InputEncoder {
public:
    enum class Key {
        Enter,
        Backspace,
        Tab,
        Escape,
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
        Insert,
        Delete,
        PageUp,
        PageDown,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12
    };
    struct Modifiers {
        bool shift = false;
        bool alt = false;
        bool control = false;
    };

    static std::string key(Key key,
                           Modifiers modifiers,
                           const TerminalModes &modes);
    static std::string text(std::string_view utf8, bool alt = false);
    // ASCII Ctrl combinations; returns empty when there is no mapping.
    static std::string control(char ascii, bool alt = false);
    // Explicit user paste: strip unsafe C0/DEL (including ESC), preserve tab,
    // normalize newlines to CR, and wrap when bracketed paste was negotiated.
    static std::string paste(std::string_view utf8, const TerminalModes &modes);
};
