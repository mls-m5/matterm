#pragma once

#include "chatclient.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the chat looks like in a terminal, as pure functions from the model to
// bytes: bubbles with a title row, the one-line editor, and the inline "live
// region" (finished bubbles are printed once into the terminal's own
// scrollback; only the live bubbles at the bottom are redrawn in place with
// relative cursor movement). No screen buffer, no I/O. See docs/CHAT.md.
namespace chat {

// The mark at the end of another person's text that is still being typed (their
// own caret is not ours to show): a filled left-pointing triangle, or `<` on a
// terminal without Unicode. It goes when the message is posted.
constexpr char32_t kLiveMarker = 0x25C0;
constexpr char32_t kLiveMarkerAscii = U'<';

struct TermCaps {
    bool truecolor = false;
    bool colors256 = false;
    bool unicode = true;
};

// --- text layout ---------------------------------------------------------

// One display line is code points [begin, end) of the text (a space at a wrap
// point belongs to no line). Words wrap at spaces; a word longer than the
// width is broken.
struct Segment {
    std::size_t begin = 0, end = 0;
};
std::vector<Segment> wrapSegments(const std::u32string &text,
                                  std::size_t width);
struct CaretPos {
    std::size_t row = 0, column = 0;
};
// Where the caret is when it sits before code point `cursor`.
CaretPos caretIn(const std::vector<Segment> &lines, std::size_t cursor);

// --- the editor ----------------------------------------------------------

// The text of the own bubble and a caret. Only characters the room accepts get
// in, so what is typed is what can be sent.
class ChatInput {
public:
    bool insert(char32_t c);
    // Pasted text: newlines and tabs become spaces, what the room does not
    // accept is skipped. Returns true when anything was inserted.
    bool insertText(const std::string &utf8);
    bool backspace();
    bool del();
    bool left();
    bool right();
    bool home();
    bool end();
    bool killLine();
    bool killWord();        // Ctrl-W, Alt+Backspace: a word back.
    bool killWordForward(); // Alt+D, Ctrl+Delete: a word forward.
    bool wordLeft();
    bool wordRight();
    void set(const std::string &utf8); // The caret goes to the end.
    std::string text() const;
    const std::u32string &codePoints() const noexcept {
        return text_;
    }
    std::size_t cursor() const noexcept {
        return cursor_;
    }

private:
    std::u32string text_;
    std::size_t cursor_ = 0;
};

// --- the view ------------------------------------------------------------

struct ViewConfig {
    TermCaps caps;
    int columns = 80;
    int rows = 24;
    std::int64_t unixMsAtZero =
        0; // Unix milliseconds at Millis 0 (host clock).
    int utcOffsetMinutes = 0;
    std::size_t maxTypersShown = 6;
    Millis bannerPulse = 4000; // The first line pulses this long, then stays.
    bool animations = true;
};

class ChatView {
public:
    explicit ChatView(ViewConfig config);

    // Bytes to write now: events that became final are printed once above the
    // region, then the region is redrawn (it moves down as lines are added).
    std::string render(ChatClient &client, const ChatInput &input, Millis now);
    // The terminal changed size: the old region is left where it is (a window
    // that reflows lines would make any other guess wrong) and a new one starts
    // below it.
    std::string resize(int columns, int rows);
    // Leaves the screen sane: ends the region, resets attributes, shows the
    // cursor.
    std::string finish();
    // A line for the person only (a command's answer), printed with the next
    // render.
    void note(const std::string &text);
    // When render() wants to be called again for an animation.
    std::optional<Millis> nextFrame(Millis now) const;

    // The lines of one bubble, for tests and for printing (each exactly as wide
    // as the bubble; `own` marks the face).
    struct BubbleSpec {
        std::string name;
        std::string text;
        std::string state; // "sent", "typing...", "draft sent", "not sent"
        bool own = false;
        bool live = false;        // A bubble still being typed.
        bool marker = false;      // Show the live marker after the text.
        bool markerBright = true; // The marker pulses between bright and dim.
        std::optional<Millis> at; // Shown as HH:MM when set.
        std::size_t cursor = 0;   // Own bubble: caret index (code points).
    };
    struct Rendered {
        std::vector<std::string> lines;
        std::size_t caretRow = 0, caretColumn = 0; // Within `lines`.
    };
    Rendered bubble(const BubbleSpec &spec) const;

private:
    std::string line(const std::string &text,
                     bool dim = false,
                     bool bold = false) const;
    std::string clock(Millis at) const;
    std::vector<std::string> printable(ChatClient &client, Millis now);
    std::string banner(Millis now) const;

    ViewConfig config_;
    std::size_t regionRows_ = 0; // Lines currently drawn, 0 before the first.
    std::size_t cursorRow_ = 0;  // Where the caret is, from the region's top.
    ChatClient::State lastState_ = ChatClient::State::Off;
    bool bannerPrinted_ = false;
    Millis start_ = -1;
    std::size_t liveOthers_ =
        0; // Typers drawn by the last render (for the pulse).
    std::vector<std::string> notes_;
};

} // namespace chat
