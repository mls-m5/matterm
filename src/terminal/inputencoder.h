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

    // Mouse report input. Coordinates are zero-based text cells.
    struct MouseEvent {
        enum class Type { Press, Release, Motion };
        enum class Button {
            Left,
            Middle,
            Right,
            None, // Motion with no button held.
            WheelUp,
            WheelDown,
            WheelLeft,
            WheelRight
        };
        Type type = Type::Press;
        Button button = Button::Left;
        Modifiers modifiers;
        int column = 0;
        int row = 0;
    };

    // Returns empty when the event should not be reported under the current
    // tracking/encoding modes (including coordinates the encoding cannot
    // express). Wheel events have no release.
    static std::string mouse(const MouseEvent &event,
                             const TerminalModes &modes);
    // Focus in/out report when focusReporting was negotiated, else empty.
    static std::string focus(bool gained, const TerminalModes &modes);
    // Keypad key ('0'..'9', '+', '-', '*', '/', '.', ',', '=' or '\r' for
    // Enter). Application keypad mode sends SS3 sequences, else the character.
    static std::string keypad(char key,
                              Modifiers modifiers,
                              const TerminalModes &modes);
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
