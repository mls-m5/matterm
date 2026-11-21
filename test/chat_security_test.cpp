// Threat-model checks of the chat core: a hostile server or client trying to
// put control sequences, spoofed text or unbounded state into another person's
// terminal. Passing checks only; gaps are in chatissues_test.cpp
// (docs/CHAT_REVIEW.md).
#include "chatclient.h"
#include "chatproto.h"
#include "chatroom.h"
#include "chattext.h"
#include "chatview.h"
#include "mls-unit-test/unittest.h"

#include <random>
#include <set>
#include <string>

namespace {
using namespace chat;

// Whether bytes written to a terminal contain only what the view composes: CSI
// sequences with numeric parameters and a final byte it uses, plain text, CR
// and LF. Anything else (another escape, a C0 or C1 control, DEL) is a leak.
bool onlyComposedSequences(const std::string &bytes, std::string &why) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c == 0x1B) {
            if (i + 1 >= bytes.size() || bytes[i + 1] != '[') {
                why = "ESC not followed by [";
                return false;
            }
            std::size_t j = i + 2;
            while (j < bytes.size() &&
                   (std::isdigit(static_cast<unsigned char>(bytes[j])) ||
                    bytes[j] == ';' || bytes[j] == '?')) {
                ++j;
            }
            if (j >= bytes.size() ||
                std::string("ABGJKmhl").find(bytes[j]) == std::string::npos) {
                why = "CSI with an unexpected final byte";
                return false;
            }
            i = j;
        }
        else if ((c < 0x20 && c != '\r' && c != '\n') || c == 0x7F) {
            why = "a C0 control or DEL";
            return false;
        }
        else if (c == 0xC2 && i + 1 < bytes.size() &&
                 static_cast<unsigned char>(bytes[i + 1]) >= 0x80 &&
                 static_cast<unsigned char>(bytes[i + 1]) <= 0x9F) {
            why = "a C1 control";
            return false;
        }
    }
    return true;
}

std::string hostileText(std::mt19937 &rng) {
    static const std::string pieces[] = {
        "\x1b[2J",
        "\x1b]52;c;AAAA\x07",
        "\x1bP1$r\x1b\\",
        "\x1b[?1049h",
        "\x9b"
        "31m",
        "\xc2\x9b"
        "2J",
        "\xe2\x80\xae",
        "\xe2\x80\x8b",
        "\xef\xbb\xbf",
        "\xf3\xa0\x80\x81",
        "\r\n",
        "\t",
        "\x7f",
        "\x1b",
        "[31m",
        "plain",
        "\xc3\xa5\xc3\xa4\xc3\xb6",
        "\xff\xfe",
        "\xed\xa0\x80",
        "\xf4\x90\x80\x80",
    };
    std::string out;
    const auto count = rng() % 6 + 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (rng() % 17 == 0) {
            out += '\0'; // A NUL cannot sit in a string literal.
        }
        out += pieces[rng() % std::size(pieces)];
    }
    return out;
}

struct Joined {
    ChatClient client{[] {
        ChatClient::Config c;
        c.name = "ada";
        return c;
    }()};
    ChatInput input;
    Millis now = 0;
    Joined() {
        client.connect(now);
        client.opened(now);
        client.receive("welcome\tada", now);
        client.receive("users\tada", now);
        client.receive("ready", now);
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatSecurity)

TEST_CASE(
    "A hostile server cannot put its own control sequences on the screen") {
    std::mt19937 rng(20261121);
    Joined j;
    ViewConfig config;
    config.animations = false;
    ChatView view(config);
    std::string all = view.render(j.client, j.input, j.now);
    for (int i = 0; i < 4000; ++i) {
        const auto text = hostileText(rng);
        std::string frame;
        switch (rng() % 7) {
        case 0:
            frame = "msg\t" + std::to_string(i + 1) + "\tbo\t" + text;
            break;
        case 1:
            frame = "draft\tbo\t" + text;
            break;
        case 2:
            frame = "hist\t" + std::to_string(i + 1) + "\t5\tcy\t" + text;
            break;
        case 3:
            frame = "notice\t" + text;
            break;
        case 4:
            frame = "error\tbad-line\t" + text;
            break;
        case 5:
            frame = "join\tn" + std::to_string(i % 50);
            break;
        default:
            frame = text; // Not even a known line.
        }
        j.client.receive(frame, j.now);
        j.now += 100;
        all += view.render(j.client, j.input, j.now);
    }
    std::string why;
    EXPECT(onlyComposedSequences(all, why));
    if (!why.empty()) {
        std::cout << "leak: " << why << "\n";
    }
    EXPECT(all.size() > 1000); // Something was actually drawn.
    view.note("local \x1b]0;title\x07 note");
    std::string noted = view.render(j.client, j.input, j.now);
    EXPECT(onlyComposedSequences(noted, why));
}

TEST_CASE("Text typed or pasted by the person is filtered like text received") {
    ChatInput input;
    // A bracketed-paste end marker and other escapes in pasted text: the
    // controls are skipped, so only harmless letters are left.
    input.insertText("before\x1b[201~\x1b]0;x\x07\xc2\x9b"
                     "31m after\r\nnext\tline");
    const auto text = input.text();
    EXPECT(text.find('\x1b') == std::string::npos);
    EXPECT(text.find('\x07') == std::string::npos);
    EXPECT(text.find("\xc2\x9b") == std::string::npos);
    EXPECT(checkDraft(text).ok());
    EXPECT(text.find("before") != std::string::npos);
    EXPECT(text.find("after") != std::string::npos);
    // Broken UTF-8 makes the whole paste be refused, which is safe.
    ChatInput broken;
    EXPECT(!broken.insertText("ok\xff"
                              "bad"));
    EXPECT(broken.text().empty());
}

TEST_CASE("The room never repeats what a client sent inside an error") {
    ChatRoom room;
    Millis now = 0;
    room.connected(1, "s1", now);
    std::set<std::string> texts;
    std::mt19937 rng(7);
    for (int i = 0; i < 400; ++i) {
        now += 50;
        const auto hostile = hostileText(rng);
        const std::string frames[] = {"hello\t" + hostile,
                                      "nick\t" + hostile,
                                      "draft\t1\t" + hostile,
                                      "lock\t" + hostile,
                                      hostile};
        for (const auto &frame : frames) {
            for (const auto &action : room.line(1, frame, now)) {
                const auto parsed = parseServerLine(action.line);
                if (action.close || parsed.error != ProtoError::None) {
                    continue;
                }
                if (parsed.line.kind == ServerLine::Kind::Error) {
                    texts.insert(parsed.line.text);
                }
            }
        }
        if (room.stats().connections == 0) {
            room.connected(1, "s" + std::to_string(i), now);
        }
    }
    // A handful of fixed sentences, none containing the attacker's bytes.
    EXPECT(texts.size() <= 12);
    for (const auto &text : texts) {
        EXPECT(text.find("plain") == std::string::npos);
        EXPECT(text.find('[') == std::string::npos);
    }
}

TEST_CASE(
    "Names that are not plain ASCII never get in, however they are spelled") {
    for (const char *name : {"ad\xc3\xa5",
                             "\xd0\xb0\x64\x61",
                             "ada\xe2\x80\x8b",
                             "a\xcc\x88"
                             "da",
                             "\xef\xbc\xa1\xef\xbc\xa4",
                             "ada\n",
                             "ada ",
                             " ada",
                             "a\0da",
                             "ad\x7f",
                             "\xe2\x80\xae"
                             "ada",
                             "ad\xc2\xa0"
                             "a",
                             "ada\x1b[0m",
                             "ada;rm",
                             "ada,bo",
                             "../ada",
                             "ada\tbo"}) {
        EXPECT(checkName(std::string_view(
                   name, std::char_traits<char>::length(name))) !=
               NameError::None);
    }
    EXPECT(checkName(std::string_view("a\0da", 4)) != NameError::None);
}

TEST_CASE("A client that floods one long line or many tiny ones is bounded") {
    ChatClient::Config config;
    config.name = "ada";
    ChatClient client(config);
    client.connect(0);
    client.opened(0);
    client.receive("welcome\tada", 0);
    client.receive("ready", 0);
    for (int i = 0; i < 20000; ++i) {
        client.receive("join\tuser" + std::to_string(i), i);
        client.receive("draft\tuser" + std::to_string(i) + "\thello", i);
        client.receive("msg\t" + std::to_string(i + 1) + "\tuser" +
                           std::to_string(i % 10) + "\ttext",
                       i);
    }
    EXPECT(client.users().size() <= config.maxUsers);
    EXPECT(client.typers().size() <= 16);
    EXPECT(client.history().size() <= config.maxHistory);
    EXPECT(client.takeEvents().size() <= config.maxEvents);
}

TEST_SUIT_END
