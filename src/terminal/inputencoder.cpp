#include "inputencoder.h"

std::string InputEncoder::text(std::string_view utf8, bool alt) {
    return (alt ? std::string("\x1b") : std::string{}) + std::string(utf8);
}
std::string InputEncoder::control(char ascii, bool alt) {
    const auto ch = static_cast<unsigned char>(ascii);
    if (ch >= 'a' && ch <= 'z') {
        return text(std::string(1, char(ch - 'a' + 1)), alt);
    }
    if (ch >= '@' && ch <= '_') {
        return text(std::string(1, char(ch & 0x1f)), alt);
    }
    if (ch == ' ') {
        return text(std::string(1, '\0'), alt);
    }
    if (ch == '?') {
        return text(std::string(1, '\x7f'), alt);
    }
    return {};
}
std::string InputEncoder::key(Key key,
                              Modifiers mod,
                              const TerminalModes &modes) {
    const int modifier =
        1 + int(mod.shift) + 2 * int(mod.alt) + 4 * int(mod.control);
    switch (key) {
    case Key::Enter:
        return text("\r", mod.alt);
    case Key::Backspace:
        return text(mod.control ? "\b" : "\x7f", mod.alt);
    case Key::Tab:
        return mod.shift ? "\x1b[Z" : text("\t", mod.alt);
    case Key::Escape:
        return text("\x1b", mod.alt);
    default:
        break;
    }
    char final = 0;
    switch (key) {
    case Key::Up:
        final = 'A';
        break;
    case Key::Down:
        final = 'B';
        break;
    case Key::Right:
        final = 'C';
        break;
    case Key::Left:
        final = 'D';
        break;
    case Key::Home:
        final = 'H';
        break;
    case Key::End:
        final = 'F';
        break;
    case Key::F1:
        final = 'P';
        break;
    case Key::F2:
        final = 'Q';
        break;
    case Key::F3:
        final = 'R';
        break;
    case Key::F4:
        final = 'S';
        break;
    default:
        break;
    }
    if (final) {
        if (modifier != 1) {
            return "\x1b[1;" + std::to_string(modifier) + final;
        }
        const bool ss3 =
            modes.applicationCursor || (key >= Key::F1 && key <= Key::F4);
        return std::string(ss3 ? "\x1bO" : "\x1b[") + final;
    }
    int code = 0;
    switch (key) {
    case Key::Insert:
        code = 2;
        break;
    case Key::Delete:
        code = 3;
        break;
    case Key::PageUp:
        code = 5;
        break;
    case Key::PageDown:
        code = 6;
        break;
    case Key::F5:
        code = 15;
        break;
    case Key::F6:
        code = 17;
        break;
    case Key::F7:
        code = 18;
        break;
    case Key::F8:
        code = 19;
        break;
    case Key::F9:
        code = 20;
        break;
    case Key::F10:
        code = 21;
        break;
    case Key::F11:
        code = 23;
        break;
    case Key::F12:
        code = 24;
        break;
    default:
        break;
    }
    return code
               ? "\x1b[" + std::to_string(code) +
                     (modifier == 1 ? "" : ";" + std::to_string(modifier)) + "~"
               : "";
}
std::string InputEncoder::paste(std::string_view utf8,
                                const TerminalModes &modes) {
    std::string result = modes.bracketedPaste ? "\x1b[200~" : "";
    bool previousCr = false;
    for (const unsigned char ch : utf8) {
        if (ch == '\n') {
            if (!previousCr) {
                result += '\r';
            }
        }
        else if (ch == '\r' || ch == '\t' || (ch >= 0x20 && ch != 0x7f)) {
            result += char(ch);
        }
        previousCr = ch == '\r';
    }
    if (modes.bracketedPaste) {
        result += "\x1b[201~";
    }
    return result;
}
