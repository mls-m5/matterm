#include "chatclient.h"
#include "chatroom.h"
#include "chatview.h"
#include "mls-unit-test/unittest.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <string>

namespace {
using namespace chat;

// What a terminal shows after the view's output.
struct Term {
    explicit Term(int columns = 60, int rows = 40)
        : model(std::size_t(columns), std::size_t(rows), 200)
        , parser(model) {}
    void write(const std::string &bytes) {
        parser.feed(bytes);
    }
    std::string row(std::size_t y) const {
        std::string text;
        for (std::size_t x = 0; x < model.columns(); ++x) {
            const auto cp = model.cell(x, y).codePoint;
            if (cp < 128) {
                text.push_back(cp >= 32 ? char(cp) : ' ');
            }
            else if (cp == 0x25CF) {
                text.push_back('@'); // The own face.
            }
            else if (cp == 0xB7) {
                text.push_back('.');
            }
            else if (cp == 0x25C0) {
                text.push_back('<'); // The live marker.
            }
            else if (cp == 0x2026) {
                text.push_back('~');
            }
            else {
                text.push_back('?');
            }
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }
    std::string screen() const {
        std::string out;
        for (std::size_t y = 0; y < model.rows(); ++y) {
            out += row(y) + "\n";
        }
        return out;
    }
    bool shows(const std::string &needle) const {
        return screen().find(needle) != std::string::npos;
    }
    int count(const std::string &needle) const {
        int n = 0;
        const std::string s = screen();
        for (auto p = s.find(needle); p != std::string::npos;
             p = s.find(needle, p + 1)) {
            ++n;
        }
        return n;
    }
    TerminalModel model;
    VtParser parser;
};

ChatClient::Config named(const std::string &name) {
    ChatClient::Config c;
    c.name = name;
    return c;
}

// A client that has joined, with the room around it.
struct Joined {
    Joined()
        : client(named("mia")) {
        client.connect(0);
        client.opened(0);
        for (const char *line :
             {"welcome\tmia", "users\tmia,bo", "ready", "online\t2"}) {
            client.receive(line, now);
        }
    }
    ChatClient client;
    ChatInput input;
    Millis now = 1000;
};

ViewConfig config(int columns = 60, int rows = 40) {
    ViewConfig c;
    c.columns = columns;
    c.rows = rows;
    c.caps.truecolor = true;
    c.unixMsAtZero = 14LL * 3600 * 1000 + 6 * 60 * 1000; // 14:06 at zero.
    c.animations = false;
    return c;
}
} // namespace

TEST_SUIT_BEGIN(ChatView)

TEST_CASE("Words wrap at spaces and long words break; the caret is found") {
    auto wrap = [](const std::string &s, std::size_t w) {
        std::u32string cps;
        for (const char c : s) {
            cps += char32_t(c);
        }
        std::vector<std::string> out;
        for (const auto &seg : wrapSegments(cps, w)) {
            std::string line;
            for (auto i = seg.begin; i < seg.end; ++i) {
                line += char(cps[i]);
            }
            out.push_back(line);
        }
        return out;
    };
    EXPECT(wrap("hello world", 20) == std::vector<std::string>{"hello world"});
    EXPECT(wrap("hello world", 8) ==
           (std::vector<std::string>{"hello", "world"}));
    EXPECT(wrap("abcdefghij", 4) ==
           (std::vector<std::string>{"abcd", "efgh", "ij"}));
    EXPECT(wrap("", 5) == std::vector<std::string>{""});
    EXPECT(wrap("ab cd ef", 5) == (std::vector<std::string>{"ab cd", "ef"}));
    std::u32string t = U"hello world";
    const auto segs = wrapSegments(t, 8);
    EXPECT(caretIn(segs, 0).row == 0);
    EXPECT(caretIn(segs, 5).row == 0); // At the end of "hello".
    EXPECT(caretIn(segs, 5).column == 5);
    EXPECT(caretIn(segs, 6).row == 1); // Before "world".
    EXPECT(caretIn(segs, 6).column == 0);
    EXPECT(caretIn(segs, 11).row == 1);
    EXPECT(caretIn(segs, 11).column == 5);
}

TEST_CASE("The editor only takes what the room accepts") {
    ChatInput in;
    EXPECT(in.insertText("hello"));
    EXPECT(!in.insert(U'\x1b'));
    EXPECT(!in.insert(0x1F600)); // An emoji.
    EXPECT(in.insert(0xE5));     // a-ring
    EXPECT(in.text() == "hello\xC3\xA5");
    EXPECT(in.left());
    EXPECT(in.insert(U'!'));
    EXPECT(in.text() == "hello!\xC3\xA5");
    EXPECT(in.backspace());
    EXPECT(in.home());
    EXPECT(!in.left());
    EXPECT(in.del());
    EXPECT(in.text() == "ello\xC3\xA5");
    in.set("one two three");
    EXPECT(in.killWord());
    EXPECT(in.text() == "one two ");
    EXPECT(in.killLine());
    EXPECT(in.text().empty());
    // A paste: newlines become spaces, the rest that is not allowed is skipped.
    EXPECT(in.insertText("line one\nline\ttwo \xF0\x9F\x98\x80 \x1b[31m!"));
    EXPECT(in.text() == "line one line two  [31m!");
    // The 200 character limit and the repeat rule hold.
    in.set("");
    EXPECT(!in.insertText(std::string(300, 'x')) ||
           in.codePoints().size() <= 200);
    EXPECT(in.codePoints().size() <=
           std::size_t(8)); // Longer runs are refused.
}

TEST_CASE(
    "A bubble has a title row, padding and a width that fits the content") {
    ChatView view(config());
    ChatView::BubbleSpec spec;
    spec.name = "bo";
    spec.text = "hi everyone";
    spec.state = "sent";
    spec.at = 0;
    const auto b = view.bubble(spec);
    EXPECT(b.lines.size() == std::size_t(2));
    Term t;
    for (const auto &l : b.lines) {
        t.write(l + "\r\n");
    }
    EXPECT(t.row(0) == " bo  14:06  sent");
    EXPECT(t.row(1) == " hi everyone");
    // The cells of the title row and the body have the same width.
    std::size_t titleWidth = 0, bodyWidth = 0;
    for (std::size_t x = 0; x < 60; ++x) {
        titleWidth += t.model.cell(x, 0).background.r +
                          t.model.cell(x, 0).background.g +
                          t.model.cell(x, 0).background.b !=
                      0;
        bodyWidth += t.model.cell(x, 1).background.r +
                         t.model.cell(x, 1).background.g +
                         t.model.cell(x, 1).background.b !=
                     0;
    }
    EXPECT(titleWidth == bodyWidth);
    EXPECT(titleWidth >= std::size_t(16));
    // Long text wraps inside the maximum width.
    spec.text = std::string(150, 'a');
    for (std::size_t i = 7; i < spec.text.size(); i += 8) {
        spec.text[i] = ' ';
    }
    EXPECT(view.bubble(spec).lines.size() >= std::size_t(4));
}

TEST_CASE(
    "Finished bubbles are printed once; the live region is redrawn below") {
    Joined j;
    ChatView view(config());
    Term t;
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.shows("Mattrix chat . 2 here"));
    EXPECT(t.shows("@ mia  ready"));
    // Someone else types, then locks.
    j.now += 100;
    j.client.receive("draft\tbo\tthinking about it", j.now);
    for (int i = 0; i < 60; ++i) { // The text appears a character at a time.
        j.now += 50;
        j.client.tick(j.now);
    }
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.shows("bo  typing~"));
    EXPECT(t.shows("thinking about it"));
    j.now += 100;
    j.client.receive("msg\t1\tbo\tthinking about it", j.now);
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.count("thinking about it") == 1); // Finished: printed once...
    EXPECT(!t.shows("typing"));                // ...and no longer live.
    EXPECT(t.shows("bo  14:06  sent"));
    // Another message pushes the region down and nothing is duplicated.
    j.client.receive("msg\t2\tbo\tsecond", j.now);
    t.write(view.render(j.client, j.input, j.now));
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.count("Mattrix chat") == 1);
    EXPECT(t.count("@ mia  ready") == 1);
    EXPECT(t.count("second") == 1);
}

TEST_CASE("The own bubble shows what is typed, wraps, and the caret follows "
          "the text") {
    Joined j;
    ChatView view(config(40, 30));
    Term t(40, 30);
    j.input.insertText("hello world, this is a fairly long message that wraps");
    ChatClient::Out ignored;
    (void)ignored;
    ChatClient::Out out;
    j.client.edit(j.input.text(), j.now, out);
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.shows("@ mia  draft sent"));
    EXPECT(t.shows("hello world,"));
    EXPECT(t.shows("wraps"));
    // The caret is at the end of the last line of the text.
    const auto cursor = t.model.cursor();
    EXPECT(t.row(cursor.row).find("wraps") != std::string::npos);
    EXPECT(std::size_t(cursor.column) == t.row(cursor.row).find("wraps") + 5);
}

TEST_CASE("Editing in the middle moves the caret there, not to the end") {
    Joined j;
    ChatView view(config());
    Term t;
    j.input.insertText("abcdef");
    j.input.left();
    j.input.left();
    t.write(view.render(j.client, j.input, j.now));
    const auto cursor = t.model.cursor();
    EXPECT(t.row(cursor.row).find("abcdef") != std::string::npos);
    EXPECT(std::size_t(cursor.column) == t.row(cursor.row).find("abcdef") + 4);
    // Typing then redraws in place without leaving the old region behind.
    j.input.insert(U'X');
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.count("@ mia") == 1);
    EXPECT(t.shows("abcdXef"));
}

TEST_CASE(
    "The region never grows past the screen and stays tidy on a short one") {
    Joined j;
    ChatView view(config(50, 12));
    Term t(50, 12);
    for (int i = 0; i < 8; ++i) {
        j.client.receive("draft\tuser" + std::to_string(i) + "\tsome words " +
                             std::to_string(i),
                         j.now);
        j.client.receive("join\tuser" + std::to_string(i), j.now);
    }
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.shows("@ mia"));
    // Printing messages scrolls the screen; the region is still drawn once.
    for (int i = 0; i < 6; ++i) {
        j.client.receive("msg\t" + std::to_string(i + 1) +
                             "\tbo\tmessage number " + std::to_string(i),
                         j.now);
        t.write(view.render(j.client, j.input, j.now));
    }
    EXPECT(t.count("@ mia") == 1);
    EXPECT(t.count("Mattrix chat") == 1);
    EXPECT(t.shows("message number 5"));
}

TEST_CASE("A note is printed once above the region, and cannot carry escapes") {
    Joined j;
    ChatView view(config());
    Term t;
    t.write(view.render(j.client, j.input, j.now));
    view.note("here: ada, bo \x1b[2J");
    t.write(view.render(j.client, j.input, j.now));
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.count("here: ada, bo") == 1);
    EXPECT(t.count("@ mia  ready") == 1);
    EXPECT(t.shows("here: ada, bo [2J"));
}

TEST_CASE("Other people's live text ends in a marker; yours and finished ones "
          "do not") {
    Joined j;
    ViewConfig c = config();
    c.animations = true;
    ChatView view(c);
    Term t;
    j.client.receive("draft\tbo\tthinking", j.now);
    for (int i = 0; i < 40; ++i) {
        j.now += 50;
        j.client.tick(j.now);
    }
    j.input.insertText("mine");
    const std::string raw = view.render(j.client, j.input, j.now);
    t.write(raw);
    EXPECT(t.shows("thinking<")); // Right after the text.
    EXPECT(raw.find("\xE2\x97\x80") != std::string::npos); // U+25C0
    EXPECT_EQ(t.count("<"), 1); // Not on the own bubble.
    EXPECT(t.shows("mine") && !t.shows("mine<"));
    EXPECT(kLiveMarker == char32_t(0x25C0));
    // It pulses: another phase, other bytes; the text itself is unchanged.
    const std::string later = view.render(j.client, j.input, j.now + 600);
    EXPECT(later != raw);
    EXPECT(later.find("\xE2\x97\x80") != std::string::npos);
    EXPECT(view.nextFrame(j.now + 600)
               .has_value()); // Frames are wanted for the pulse.
    // Posted: it becomes a finished bubble, without the marker.
    j.client.receive("msg\t1\tbo\tthinking", j.now);
    j.client.receive("gone\tbo", j.now);
    t.write(view.render(j.client, j.input, j.now + 1200));
    EXPECT_EQ(t.count("<"), 0);
    EXPECT(t.shows("bo  14:06  sent") || t.shows("14:06  sent"));
}

TEST_CASE("Without Unicode the marker is a plain <") {
    Joined j;
    ViewConfig c = config();
    c.caps = TermCaps{false, false, false};
    ChatView view(c);
    Term t;
    j.client.receive("draft\tbo\thi", j.now);
    for (int i = 0; i < 20; ++i) {
        j.now += 50;
        j.client.tick(j.now);
    }
    const std::string raw = view.render(j.client, j.input, j.now);
    t.write(raw);
    EXPECT(t.shows("hi<"));
    EXPECT(raw.find("\xE2\x97\x80") == std::string::npos);
    EXPECT(kLiveMarkerAscii == U'<');
}

TEST_CASE("A resize starts a new region below the old one") {
    Joined j;
    ChatView view(config());
    Term t;
    t.write(view.render(j.client, j.input, j.now));
    t.write(view.resize(50, 30));
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.count("@ mia  ready") ==
           2); // The old one stays, a new one follows.
    t.write(view.finish());
}

TEST_CASE("Without color or Unicode the bubbles still read") {
    Joined j;
    ViewConfig c = config();
    c.caps = TermCaps{false, false, false};
    ChatView view(c);
    Term t;
    j.client.receive("draft\tbo\thi", j.now);
    t.write(view.render(j.client, j.input, j.now));
    EXPECT(t.shows(":) mia  ready"));
    EXPECT(t.shows("bo  typing..."));
    EXPECT(t.screen().find('?') == std::string::npos);
    const std::string raw = view.render(j.client, j.input, j.now);
    EXPECT(raw.find("38;2;") == std::string::npos);
    EXPECT(raw.find("38;5;") == std::string::npos);
    EXPECT(raw.find("\x1b[7m") !=
           std::string::npos); // Reverse video for titles.
}

TEST_CASE("The banner pulses and then stays as a plain line") {
    Joined j;
    ViewConfig c = config();
    c.animations = true;
    ChatView view(c);
    Term t;
    t.write(view.render(j.client, j.input, 0));
    EXPECT(t.shows("Live chat, draft sent as you type"));
    EXPECT(view.nextFrame(0).has_value());
    t.write(view.render(j.client, j.input, 2000));
    EXPECT(t.count("Live chat, draft sent as you type") == 1);
    t.write(view.render(j.client, j.input, 4500));
    EXPECT(t.count("Live chat, draft sent as you type") ==
           1); // Printed, static.
    EXPECT(!view.nextFrame(4500).has_value());
    t.write(view.render(j.client, j.input, 9000));
    EXPECT(t.count("Live chat, draft sent as you type") == 1);
}

TEST_CASE(
    "Hostile text in the model cannot put escape sequences on the screen") {
    Joined j;
    ChatView view(config());
    Term t;
    j.client.receive("msg\t1\tbo\t\xC2\x9B"
                     "31m\xE2\x80\xAE"
                     "evil",
                     j.now);
    j.client.receive("notice\t\xFF"
                     "x",
                     j.now);
    const std::string raw = view.render(j.client, j.input, j.now);
    // Apart from the view's own sequences there is no ESC followed by anything
    // that is not a CSI SGR/cursor sequence, and no OSC or C1 introducer.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\x1b') {
            EXPECT(i + 1 < raw.size() && raw[i + 1] == '[');
        }
        EXPECT(static_cast<unsigned char>(raw[i]) != 0x9B);
    }
    EXPECT(raw.find("\x1b]") == std::string::npos);
    t.write(raw);
    EXPECT(t.shows("31mevil"));
}

TEST_SUIT_END
