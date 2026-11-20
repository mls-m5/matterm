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
namespace {
void appendUtf8(std::string &out, int value) {
    if (value < 0x80) {
        out += char(value);
    }
    else {
        out += char(0xc0 | (value >> 6));
        out += char(0x80 | (value & 0x3f));
    }
}
} // namespace

std::string InputEncoder::mouse(const MouseEvent &event,
                                const TerminalModes &modes) {
    using Tracking = TerminalModes::MouseTracking;
    using Encoding = TerminalModes::MouseEncoding;
    using Button = MouseEvent::Button;
    using Type = MouseEvent::Type;
    if (modes.mouseTracking == Tracking::Off || event.column < 0 ||
        event.row < 0) {
        return {};
    }
    const bool wheel = event.button >= Button::WheelUp;
    if (wheel && event.type != Type::Press) {
        return {};
    }
    if (event.type == Type::Release && modes.mouseTracking == Tracking::Press) {
        return {};
    }
    if (event.type == Type::Motion) {
        const bool pressed = event.button != Button::None;
        if (wheel || modes.mouseTracking == Tracking::Press ||
            modes.mouseTracking == Tracking::Normal ||
            (modes.mouseTracking == Tracking::Drag && !pressed)) {
            return {};
        }
    }
    int code = 0;
    switch (event.button) {
    case Button::Left:
        code = 0;
        break;
    case Button::Middle:
        code = 1;
        break;
    case Button::Right:
        code = 2;
        break;
    case Button::None:
        code = 3;
        break;
    case Button::WheelUp:
        code = 64;
        break;
    case Button::WheelDown:
        code = 65;
        break;
    case Button::WheelLeft:
        code = 66;
        break;
    case Button::WheelRight:
        code = 67;
        break;
    }
    const bool release = event.type == Type::Release;
    const bool sgr = modes.mouseEncoding == Encoding::Sgr;
    if (release && !sgr) {
        code = 3; // Legacy encodings do not say which button was released.
    }
    if (modes.mouseTracking != Tracking::Press) {
        code += 4 * int(event.modifiers.shift) + 8 * int(event.modifiers.alt) +
                16 * int(event.modifiers.control);
    }
    if (event.type == Type::Motion) {
        code += 32;
    }
    const int column = event.column + 1;
    const int row = event.row + 1;
    if (sgr) {
        return "\x1b[<" + std::to_string(code) + ";" + std::to_string(column) +
               ";" + std::to_string(row) + (release ? 'm' : 'M');
    }
    if (modes.mouseEncoding == Encoding::Urxvt) {
        return "\x1b[" + std::to_string(code + 32) + ";" +
               std::to_string(column) + ";" + std::to_string(row) + "M";
    }
    const int limit = modes.mouseEncoding == Encoding::Utf8 ? 2015 : 223;
    if (column > limit || row > limit || code + 32 > limit) {
        return {};
    }
    std::string result = "\x1b[M";
    if (modes.mouseEncoding == Encoding::Utf8) {
        appendUtf8(result, code + 32);
        appendUtf8(result, column + 32);
        appendUtf8(result, row + 32);
    }
    else {
        result += char(code + 32);
        result += char(column + 32);
        result += char(row + 32);
    }
    return result;
}
std::string InputEncoder::focus(bool gained, const TerminalModes &modes) {
    return modes.focusReporting ? (gained ? "\x1b[I" : "\x1b[O") : "";
}
std::string InputEncoder::keypad(char key,
                                 Modifiers mod,
                                 const TerminalModes &modes) {
    if (!modes.applicationKeypad) {
        return key == '\r' ? InputEncoder::key(Key::Enter, mod, modes)
                           : text(std::string(1, key), mod.alt);
    }
    char final = 0;
    if (key >= '0' && key <= '9') {
        final = char('p' + (key - '0'));
    }
    else {
        switch (key) {
        case '*':
            final = 'j';
            break;
        case '+':
            final = 'k';
            break;
        case ',':
            final = 'l';
            break;
        case '-':
            final = 'm';
            break;
        case '.':
            final = 'n';
            break;
        case '/':
            final = 'o';
            break;
        case '=':
            final = 'X';
            break;
        case '\r':
            final = 'M';
            break;
        default:
            return {};
        }
    }
    const int modifier =
        1 + int(mod.shift) + 2 * int(mod.alt) + 4 * int(mod.control);
    return modifier == 1 ? std::string("\x1bO") + final
                         : "\x1bO" + std::to_string(modifier) + final;
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
        // Ctrl+Backspace deletes a word: ^W (unix-word-rubout) is what shells and
        // most line editors bind that to; plain Backspace stays DEL.
        return text(mod.control ? "\x17" : "\x7f", mod.alt);
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
