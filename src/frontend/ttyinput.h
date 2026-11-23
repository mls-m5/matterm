#pragma once

// What the user types in the host terminal of `matterm --attach-tty`, decoded:
// the bytes of an xterm like terminal (keys, SGR mouse reports, bracketed
// paste) become events that are encoded again for what the program in the pane
// asked for (application cursor keys, mouse encoding, paste markers), the same
// way the window does it (InputEncoder). Pure: no I/O, no time. A lone Escape
// cannot be told from the start of a sequence until more bytes (or nothing)
// follow, so the owner calls flush() when the input has been quiet for a
// moment.
#include "inputencoder.h"
#include "terminalmodes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frontend::ttymode {

// The key that leaves the pane for the list: Ctrl+] (0x1d), the "escape to the
// prompt" key of telnet and ssh's cousins; programs almost never use it.
inline constexpr char kLeaveByte = 0x1d;
inline constexpr const char *kLeaveName = "Ctrl+]";
// The prefix key, Ctrl+B: the same as the window's (src/frontend/prefixkeys.h).
// Twice sends one to the program.
inline constexpr char kPrefixByte = 0x02;

struct InputEvent {
    enum class Kind {
        Bytes, // Anything not decoded: sent on as it came.
        Key,
        Mouse,
        Paste,  // Text of a bracketed paste, markers removed.
        Leave,  // The leave key.
        Prefix, // Ctrl+B, the prefix of the commands (like the window's).
    };
    Kind kind = Kind::Bytes;
    std::string text; // Bytes, Paste.
    InputEncoder::Key key = InputEncoder::Key::Enter;
    InputEncoder::Modifiers modifiers;
    InputEncoder::MouseEvent mouse;
};

class HostInputDecoder {
public:
    // Decodes what is complete; an incomplete sequence at the end waits for the
    // next feed.
    std::vector<InputEvent> feed(std::string_view bytes) {
        pending_.append(bytes);
        return run(false);
    }
    // The input went quiet: what waits is not the start of a sequence any more
    // (a lone ESC is the Escape key).
    std::vector<InputEvent> flush() {
        return run(true);
    }
    bool pending() const {
        return !pending_.empty() || inPaste_;
    }

private:
    static constexpr std::size_t kMaxPaste = 8u << 20;

    static InputEvent bytes(std::string text) {
        InputEvent e;
        e.kind = InputEvent::Kind::Bytes;
        e.text = std::move(text);
        return e;
    }
    static InputEvent key(InputEncoder::Key k, InputEncoder::Modifiers m = {}) {
        InputEvent e;
        e.kind = InputEvent::Kind::Key;
        e.key = k;
        e.modifiers = m;
        return e;
    }
    static InputEncoder::Modifiers modifiersOf(int parameter) {
        InputEncoder::Modifiers m;
        const int bits = parameter > 1 ? parameter - 1 : 0;
        m.shift = (bits & 1) != 0;
        m.alt = (bits & 2) != 0;
        m.control = (bits & 4) != 0;
        return m;
    }
    // "1;5" -> {1, 5}. A missing or odd number is 0 (absent).
    static std::vector<int> numbers(std::string_view params) {
        std::vector<int> out;
        int value = 0;
        bool any = false;
        for (const char c : params) {
            if (c >= '0' && c <= '9') {
                value = std::min(value * 10 + (c - '0'), 1000000);
                any = true;
            }
            else {
                out.push_back(any ? value : 0);
                value = 0;
                any = false;
            }
        }
        out.push_back(any ? value : 0);
        return out;
    }
    static std::optional<InputEncoder::Key> tildeKey(int code) {
        using K = InputEncoder::Key;
        switch (code) {
        case 1:
        case 7:
            return K::Home;
        case 2:
            return K::Insert;
        case 3:
            return K::Delete;
        case 4:
        case 8:
            return K::End;
        case 5:
            return K::PageUp;
        case 6:
            return K::PageDown;
        case 11:
            return K::F1;
        case 12:
            return K::F2;
        case 13:
            return K::F3;
        case 14:
            return K::F4;
        case 15:
            return K::F5;
        case 17:
            return K::F6;
        case 18:
            return K::F7;
        case 19:
            return K::F8;
        case 20:
            return K::F9;
        case 21:
            return K::F10;
        case 23:
            return K::F11;
        case 24:
            return K::F12;
        default:
            return std::nullopt;
        }
    }
    static std::optional<InputEncoder::Key> letterKey(char final) {
        using K = InputEncoder::Key;
        switch (final) {
        case 'A':
            return K::Up;
        case 'B':
            return K::Down;
        case 'C':
            return K::Right;
        case 'D':
            return K::Left;
        case 'H':
            return K::Home;
        case 'F':
            return K::End;
        case 'P':
            return K::F1;
        case 'Q':
            return K::F2;
        case 'R':
            return K::F3;
        case 'S':
            return K::F4;
        default:
            return std::nullopt;
        }
    }
    // CSI < b ; x ; y M|m
    static std::optional<InputEvent> sgrMouse(std::string_view params,
                                              char final) {
        if (params.empty() || params[0] != '<') {
            return std::nullopt;
        }
        const auto n = numbers(params.substr(1));
        if (n.size() != 3 || n[1] < 1 || n[2] < 1) {
            return std::nullopt;
        }
        const int b = n[0];
        InputEvent e;
        e.kind = InputEvent::Kind::Mouse;
        auto &m = e.mouse;
        m.modifiers.shift = (b & 4) != 0;
        m.modifiers.alt = (b & 8) != 0;
        m.modifiers.control = (b & 16) != 0;
        m.column = n[1] - 1;
        m.row = n[2] - 1;
        using B = InputEncoder::MouseEvent::Button;
        const int low = b & 3;
        if ((b & 64) != 0) {
            m.button = low == 0   ? B::WheelUp
                       : low == 1 ? B::WheelDown
                       : low == 2 ? B::WheelLeft
                                  : B::WheelRight;
            m.type = InputEncoder::MouseEvent::Type::Press;
            return e;
        }
        m.button = low == 0   ? B::Left
                   : low == 1 ? B::Middle
                   : low == 2 ? B::Right
                              : B::None;
        if ((b & 32) != 0) {
            m.type = InputEncoder::MouseEvent::Type::Motion;
        }
        else {
            m.type = final == 'm' ? InputEncoder::MouseEvent::Type::Release
                                  : InputEncoder::MouseEvent::Type::Press;
        }
        return e;
    }

    std::vector<InputEvent> run(bool quiet) {
        std::vector<InputEvent> out;
        std::size_t pos = 0;
        const std::string &in = pending_;
        const auto rawRun = [&](std::size_t from, std::size_t to) {
            if (to > from) {
                out.push_back(bytes(in.substr(from, to - from)));
            }
        };
        while (pos < in.size()) {
            if (inPaste_) {
                static const std::string_view kEnd = "\x1b[201~";
                const auto at = in.find(kEnd, pos);
                if (at == std::string::npos) {
                    // Keep a possible partial end marker for the next feed.
                    const std::size_t keep =
                        std::min<std::size_t>(kEnd.size() - 1, in.size() - pos);
                    std::size_t cut = in.size();
                    for (std::size_t k = keep; k > 0; --k) {
                        if (in.compare(in.size() - k, k, kEnd.substr(0, k)) ==
                            0) {
                            cut = in.size() - k;
                            break;
                        }
                    }
                    if (paste_.size() + (cut - pos) <= kMaxPaste) {
                        paste_.append(in, pos, cut - pos);
                    }
                    pos = cut;
                    break;
                }
                if (paste_.size() + (at - pos) <= kMaxPaste) {
                    paste_.append(in, pos, at - pos);
                }
                InputEvent e;
                e.kind = InputEvent::Kind::Paste;
                e.text = std::move(paste_);
                paste_.clear();
                out.push_back(std::move(e));
                inPaste_ = false;
                pos = at + kEnd.size();
                continue;
            }
            const auto c = static_cast<unsigned char>(in[pos]);
            if (c == 0x1b) {
                if (pos + 1 >= in.size()) {
                    if (quiet) {
                        out.push_back(key(InputEncoder::Key::Escape));
                        ++pos;
                    }
                    break;
                }
                const char next = in[pos + 1];
                if (next == '[') {
                    std::size_t end = pos + 2;
                    while (end < in.size() &&
                           static_cast<unsigned char>(in[end]) >= 0x20 &&
                           static_cast<unsigned char>(in[end]) <= 0x3f) {
                        ++end;
                    }
                    if (end >= in.size()) {
                        if (quiet) { // Never finished: hand it on as it is.
                            rawRun(pos, in.size());
                            pos = in.size();
                        }
                        break;
                    }
                    const char final = in[end];
                    const std::string_view params(in.data() + pos + 2,
                                                  end - pos - 2);
                    handleCsi(out, in, pos, end, params, final);
                    pos = end + 1;
                    continue;
                }
                if (next == 'O') {
                    if (pos + 2 >= in.size()) {
                        if (quiet) {
                            rawRun(pos, in.size());
                            pos = in.size();
                        }
                        break;
                    }
                    if (const auto k = letterKey(in[pos + 2])) {
                        out.push_back(key(*k));
                    }
                    else {
                        rawRun(pos, pos + 3);
                    }
                    pos += 3;
                    continue;
                }
                rawRun(pos, pos + 2); // Alt + a key: ESC and the byte, as sent.
                pos += 2;
                continue;
            }
            if (c == static_cast<unsigned char>(kLeaveByte)) {
                InputEvent e;
                e.kind = InputEvent::Kind::Leave;
                out.push_back(e);
                ++pos;
                continue;
            }
            if (c == static_cast<unsigned char>(kPrefixByte)) {
                InputEvent e;
                e.kind = InputEvent::Kind::Prefix;
                out.push_back(e);
                ++pos;
                continue;
            }
            if (c == '\r') {
                out.push_back(key(InputEncoder::Key::Enter));
                ++pos;
                continue;
            }
            std::size_t end = pos + 1;
            while (end < in.size() && in[end] != 0x1b &&
                   in[end] != kLeaveByte && in[end] != kPrefixByte &&
                   in[end] != '\r') {
                ++end;
            }
            rawRun(pos, end);
            pos = end;
        }
        pending_.erase(0, pos);
        return out;
    }

    void handleCsi(std::vector<InputEvent> &out,
                   const std::string &in,
                   std::size_t start,
                   std::size_t end,
                   std::string_view params,
                   char final) {
        if (final == '~' && params == "200") {
            inPaste_ = true;
            paste_.clear();
            return;
        }
        if (final == '~' && params == "201") {
            return; // A stray end marker.
        }
        if ((final == 'M' || final == 'm') && !params.empty() &&
            params[0] == '<') {
            if (auto e = sgrMouse(params, final)) {
                out.push_back(std::move(*e));
            }
            return; // A mouse report we cannot read is dropped, not typed into
                    // the program.
        }
        const auto n = numbers(params);
        if (final == '~') {
            if (const auto k = tildeKey(n[0])) {
                out.push_back(key(*k,
                                  n.size() > 1 ? modifiersOf(n[1])
                                               : InputEncoder::Modifiers{}));
                return;
            }
        }
        else if (const auto k = letterKey(final)) {
            out.push_back(key(*k,
                              n.size() > 1 ? modifiersOf(n[1])
                                           : InputEncoder::Modifiers{}));
            return;
        }
        out.push_back(bytes(in.substr(start, end + 1 - start)));
    }

    std::string pending_;
    std::string paste_;
    bool inPaste_ = false;
};

// What goes to the program for a decoded event under its current modes (empty:
// nothing).
inline std::string encodeForProgram(const InputEvent &event,
                                    const TerminalModes &modes) {
    switch (event.kind) {
    case InputEvent::Kind::Bytes:
        return event.text;
    case InputEvent::Kind::Key:
        return InputEncoder::key(event.key, event.modifiers, modes);
    case InputEvent::Kind::Mouse:
        return InputEncoder::mouse(event.mouse, modes);
    case InputEvent::Kind::Paste:
        return InputEncoder::paste(event.text, modes);
    case InputEvent::Kind::Prefix:
    case InputEvent::Kind::Leave:
        break;
    }
    return {};
}

// What the host terminal must be asked to report so that the events above can
// happen.
struct HostInputModes {
    bool mouse = false; // Any mouse tracking: press/release, plus motion for
                        // Drag and Any.
    bool mouseMotion = false;
    bool mouseAnyMotion = false;
    bool bracketedPaste = true;
    bool operator==(const HostInputModes &o) const {
        return mouse == o.mouse && mouseMotion == o.mouseMotion &&
               mouseAnyMotion == o.mouseAnyMotion &&
               bracketedPaste == o.bracketedPaste;
    }
    bool operator!=(const HostInputModes &o) const {
        return !(*this == o);
    }
};

// The host terminal reports the mouse only while the program wants it, so that
// the phone's own selection and scrolling keep working otherwise. Paste is
// always bracketed here: the decoder removes the markers and the program gets
// them only if it asked.
inline HostInputModes hostModesFor(const TerminalModes &modes, bool live) {
    HostInputModes host;
    if (live && modes.mouseTracking != TerminalModes::MouseTracking::Off) {
        host.mouse = true;
        host.mouseMotion =
            modes.mouseTracking == TerminalModes::MouseTracking::Drag ||
            modes.mouseTracking == TerminalModes::MouseTracking::Any;
        host.mouseAnyMotion =
            modes.mouseTracking == TerminalModes::MouseTracking::Any;
    }
    return host;
}

} // namespace frontend::ttymode
