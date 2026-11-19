#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fakeos {

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
        Ctrl,  // `text` holds the lowercase letter: Ctrl-C is "c".
        Mouse, // SGR mouse report, see the fields below.
    };
    Type type = Type::Text;
    std::string text;
    // Mouse: zero-based cell, button (0 left, 1 middle, 2 right, 64/65 wheel),
    // press (false for release) and whether it was motion.
    int column = 0, row = 0, button = 0;
    bool press = true, motion = false;
    bool alt = false;

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

    State state_ = State::Ground;
    std::string params_;
    std::string pending_; // Partial UTF-8 sequence.
    std::size_t need_ = 0;
    bool pasting_ = false;
    bool afterCr_ = false;
};

} // namespace fakeos
