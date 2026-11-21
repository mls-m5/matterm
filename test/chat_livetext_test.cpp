#include "chatclient.h"
#include "mls-unit-test/unittest.h"

#include <string>
#include <vector>

// Other people's live text is shown a character at a time (docs/CHAT.md), by a
// schedule that only depends on the injected clock and a seed.
namespace {
using namespace chat;

struct Joined {
    explicit Joined(std::uint32_t seed = 1)
        : client(config(seed)) {
        client.connect(0);
        client.opened(0);
        for (const char *line : {"welcome\tada", "users\tada,bo", "ready"}) {
            client.receive(line, now);
        }
        client.takeEvents();
    }
    static ChatClient::Config config(std::uint32_t seed) {
        ChatClient::Config c;
        c.name = "ada";
        c.jitterSeed = seed;
        return c;
    }
    void feed(const std::string &frame) {
        client.receive(frame, now);
    }
    void advance(Millis ms) {
        const Millis end = now + ms;
        while (now < end) {
            now = std::min<Millis>(end, now + 5);
            client.tick(now);
        }
    }
    const std::string &shown() const {
        static const std::string none;
        return client.typers().empty() ? none : client.typers()[0].text;
    }
    ChatClient client;
    Millis now = 1000;
};

struct Timeline {
    std::vector<Millis> at;
    std::vector<std::string> text;
};

Timeline typeOut(const std::string &snapshot,
                 std::uint32_t seed = 1,
                 Millis until = 4000) {
    Joined j(seed);
    Timeline t;
    j.feed("draft\tbo\t" + snapshot);
    std::string last;
    for (Millis ms = 0; ms <= until; ms += 5) {
        j.now = 1000 + ms;
        j.client.tick(j.now);
        if (j.shown() != last) {
            last = j.shown();
            t.at.push_back(ms);
            t.text.push_back(last);
        }
    }
    return t;
}
} // namespace

TEST_SUIT_BEGIN(ChatLiveText)

TEST_CASE(
    "Text appears one character at a time, within the floor and ceiling") {
    const auto t = typeOut("hello world");
    EXPECT(t.text.size() == std::size_t(11));
    EXPECT(t.text.front() == "h"); // The first at once.
    EXPECT(t.at.front() <= 5);
    EXPECT(t.text.back() == "hello world");
    for (std::size_t i = 1; i < t.text.size(); ++i) {
        EXPECT(t.text[i].size() == t.text[i - 1].size() + 1);
        const Millis gap = t.at[i] - t.at[i - 1];
        EXPECT(gap >= 20 && gap <= 150); // 25..120 ms with a little unevenness.
    }
}

TEST_CASE("The schedule is deterministic and the seed changes the unevenness") {
    const auto a = typeOut("the same text twice");
    const auto b = typeOut("the same text twice");
    EXPECT(a.at == b.at);
    const auto c = typeOut("the same text twice", 99);
    EXPECT(c.at.size() == a.at.size());
    EXPECT(c.at != a.at);
}

TEST_CASE("A long snapshot never lags more than about a second and a half") {
    std::string text;
    for (int i = 0; i < 40; ++i) {
        text += "wor" + std::to_string(i % 10) + " ";
    }
    text.resize(180);
    const auto t = typeOut(text);
    EXPECT(t.text.back() == text);
    EXPECT(t.at.back() <= 1800);
}

TEST_CASE("A backspace on the other side is shown as one, never as a jump") {
    Joined j;
    j.feed("draft\tbo\thello world");
    j.advance(2000);
    EXPECT(j.shown() == "hello world");
    j.feed("draft\tbo\thello");
    std::string previous = j.shown();
    int removed = 0;
    for (int i = 0; i < 400; ++i) {
        j.advance(5);
        if (j.shown() != previous) {
            EXPECT(j.shown().size() + 1 == previous.size());
            EXPECT(previous.compare(0, j.shown().size(), j.shown()) == 0);
            ++removed;
            previous = j.shown();
        }
    }
    EXPECT_EQ(removed, 6);
    j.feed("draft\tbo\thelp me"); // A change in the middle: back, then forward.
    j.advance(3000);
    EXPECT(j.shown() == "help me");
}

TEST_CASE("Posted: the rest is typed quickly, then the message appears") {
    Joined j;
    const std::string text = "this is a fairly long thought";
    j.feed("draft\tbo\t" + text);
    j.advance(300);
    const auto shown = j.shown().size();
    EXPECT(shown > std::size_t(0) && shown < text.size());
    j.client.takeEvents();
    j.feed("msg\t5\tbo\t" + text);
    j.feed("gone\tbo"); // As the room sends it: it must not cut the catch-up
                        // short.
    EXPECT(j.client.history().empty());
    EXPECT(j.client.typers().size() == std::size_t(1));
    const Millis remaining = Millis(text.size() - shown) * 10;
    j.advance(remaining - 60);
    EXPECT(j.client.history().empty());
    j.advance(130);
    EXPECT(j.client.history().size() == std::size_t(1));
    EXPECT(j.client.typers().empty());
    int messages = 0;
    for (const auto &e : j.client.takeEvents()) {
        messages += e.kind == ChatClient::Event::Kind::Message;
    }
    EXPECT_EQ(messages, 1);
}

TEST_CASE("Messages keep their order behind one that is still catching up") {
    Joined j;
    const std::string text = "slow talker with a long text here";
    j.feed("draft\tbo\t" + text);
    j.advance(200);
    j.feed("msg\t5\tbo\t" + text);
    j.feed("msg\t6\tcy\tquick reply");
    EXPECT(j.client.history().empty()); // It waits for the one before it.
    j.advance(500);
    EXPECT(j.client.history().size() == std::size_t(2));
    EXPECT(j.client.history()[0].serial == 5u);
    EXPECT(j.client.history()[1].serial == 6u);
    j.feed("msg\t7\tcy\timmediate"); // Nothing waits any more: at once.
    EXPECT(j.client.history().size() == std::size_t(3));
}

TEST_CASE("Closing, leaving and a hidden message while one waits") {
    Joined j;
    j.feed("draft\tbo\ta message that was being typed");
    j.advance(100);
    j.feed("msg\t5\tbo\ta message that was being typed");
    j.feed("hide\t5"); // A moderator removed it before it showed.
    EXPECT(j.client.history().empty());
    j.advance(1000);
    EXPECT(j.client.history().empty()); // And it never appears.
    Joined k;
    k.feed("draft\tbo\tanother one being typed");
    k.advance(100);
    k.feed("msg\t5\tbo\tanother one being typed");
    EXPECT(k.client.history().empty());
    k.client.closed(k.now);
    EXPECT(k.client.history().size() == std::size_t(1));
}

TEST_CASE("The next deadline follows the next character") {
    Joined j;
    EXPECT(!j.client.nextDeadline().has_value());
    j.feed("draft\tbo\tabc");
    const auto d = j.client.nextDeadline();
    EXPECT(d.has_value());
    EXPECT(*d <= j.now + 1);
    j.advance(3000);
    EXPECT(!j.client.nextDeadline().has_value());
    EXPECT(j.client.takeTypersChanged());
    EXPECT(!j.client.takeTypersChanged());
}

TEST_SUIT_END
