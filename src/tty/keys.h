#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tty {

// One decoded unit of terminal input.
struct Key {
    enum class Type {
        Text, // `text` holds one UTF-8 code point (possibly pasted).
        Enter,
        Backspace,
        Delete,
        Tab,
        Escape,
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
        PageUp,
        PageDown,
        Insert,
        Function, // F1..F12 in `number`.
        Ctrl,     // `text` holds the lowercase letter: Ctrl-C is "c".
        Mouse,    // SGR mouse report, see the fields below.
    };
    Type type = Type::Text;
    std::string text;
    // Mouse: zero-based cell, button (0 left, 1 middle, 2 right, 64/65 wheel),
    // press (false for release) and whether it was motion.
    int column = 0, row = 0, button = 0;
    bool press = true, motion = false;
    bool alt = false;
    bool ctrl = false;  // From a CSI modifier (Ctrl+Left, Ctrl+Delete, ...).
    bool shift = false; // From a CSI modifier, or Shift+Tab (CSI Z).
    int number = 0;     // Function: 1..12.
    // The exact bytes this key came from (an escape sequence, or the UTF-8
    // character), so that a program that forwards input can send it on
    // unchanged.
    std::string raw;

    bool is(Type t) const {
        return type == t;
    }
    bool isText(std::string_view s) const {
        return type == Type::Text && text == s;
    }
};

// Incremental decoder for what a terminal sends: UTF-8 text, control keys,
// CSI/SS3 keys, SGR mouse reports and bracketed paste. Replies that are not
// keys (cursor position reports, OSC) are dropped. A lone ESC at the end of a
// chunk is the Escape key; terminals send escape sequences in one write.
class KeyDecoder {
public:
    std::vector<Key> feed(std::string_view bytes);

private:
    enum class State { Ground, Esc, Csi, Ss3, String, StringEsc };
    void finishCsi(std::vector<Key> &out);
    void textByte(unsigned char byte, std::vector<Key> &out);
    void step(unsigned char byte, bool last, std::vector<Key> &out);
    void finishUnit(std::vector<Key> &out, std::size_t before);

    State state_ = State::Ground;
    std::string params_;
    std::string pending_; // Partial UTF-8 sequence.
    std::size_t need_ = 0;
    bool pasting_ = false;
    std::string raw_; // Bytes of the key being read.
    bool afterCr_ = false;
};

} // namespace tty
