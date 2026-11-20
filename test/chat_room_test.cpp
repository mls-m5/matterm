#include "chatproto.h"
#include "chatroom.h"
#include "chattext.h"
#include "mls-unit-test/unittest.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace chat;

// A room with a clock and a mailbox per connection.
struct Net {
    explicit Net(RoomConfig config = {})
        : room(config) {}
    ChatRoom room;
    Millis now = 1000;
    std::map<ConnId, std::vector<std::string>> mail;
    std::set<ConnId> closed;

    void apply(const Actions &actions) {
        for (const auto &a : actions) {
            if (a.close) {
                closed.insert(a.conn);
            }
            else {
                EXPECT(a.line.size() <= kMaxFrameBytes);
                EXPECT(parseServerLine(a.line).error == ProtoError::None);
                mail[a.conn].push_back(a.line);
            }
        }
    }
    void connect(ConnId id, const std::string &source = "10.0.0.1") {
        apply(room.connected(id, source, now));
    }
    void join(ConnId id,
              const std::string &name,
              const std::string &source = "") {
        connect(id, source.empty() ? "src" + std::to_string(id) : source);
        say(id, "hello\t" + name);
    }
    void say(ConnId id, const std::string &frame) {
        apply(room.line(id, frame, now));
    }
    void advance(Millis ms) {
        const Millis end = now + ms;
        while (now < end) {
            now = std::min<Millis>(end, now + 50);
            apply(room.tick(now));
        }
    }
    void clear() {
        mail.clear();
    }
    bool got(ConnId id, const std::string &prefix) const {
        const auto it = mail.find(id);
        if (it == mail.end()) {
            return false;
        }
        for (const auto &l : it->second) {
            if (l.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return false;
    }
    int count(ConnId id, const std::string &prefix) const {
        int n = 0;
        const auto it = mail.find(id);
        if (it != mail.end()) {
            for (const auto &l : it->second) {
                n += l.compare(0, prefix.size(), prefix) == 0;
            }
        }
        return n;
    }
    std::string last(ConnId id, const std::string &prefix) const {
        std::string found;
        const auto it = mail.find(id);
        if (it != mail.end()) {
            for (const auto &l : it->second) {
                if (l.compare(0, prefix.size(), prefix) == 0) {
                    found = l;
                }
            }
        }
        return found;
    }
};

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return std::uint32_t(s >> 33);
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatRoom)

TEST_CASE(
    "Joining: welcome, names, history, ready, count; the others hear of it") {
    Net n;
    n.join(1, "ada");
    EXPECT(n.got(1, "welcome\tada"));
    EXPECT(n.got(1, "users\tada"));
    EXPECT(n.got(1, "ready"));
    EXPECT(n.got(1, "online\t1"));
    n.clear();
    n.join(2, "bo");
    EXPECT(n.got(2, "users\tada,bo"));
    EXPECT(n.got(1, "join\tbo"));
    EXPECT(!n.got(2, "join\t"));
    EXPECT_EQ(n.room.stats().online, std::size_t(2));
}

TEST_CASE(
    "Names: unique without regard to case, rules apply, a retry is allowed") {
    Net n;
    n.join(1, "Ada");
    n.connect(2);
    n.say(2, "hello\tada");
    EXPECT(n.got(2, "error\tname-taken"));
    EXPECT(n.closed.count(2) == 0);
    n.say(2, "hello\tAdmin");
    EXPECT(n.got(2, "error\tbad-line")); // Not a valid name: not understood.
    n.say(2, "hello\tbo");
    EXPECT(n.got(2, "welcome\tbo"));
    // Talking before hello is a mistake.
    n.connect(3);
    n.say(3, "draft\t1\thi");
    EXPECT(n.got(3, "error\tbad-line"));
    // Leaving frees the name.
    n.apply(n.room.closed(1, n.now));
    n.connect(4);
    n.say(4, "hello\tada");
    EXPECT(n.got(4, "welcome\tada"));
    EXPECT(n.got(2, "leave\tAda"));
}

TEST_CASE("Hello must come in time") {
    Net n;
    n.connect(1);
    n.advance(4000);
    EXPECT(n.closed.count(1) == 0);
    n.advance(1500);
    EXPECT(n.closed.count(1) == 1);
    EXPECT(n.got(1, "error"));
    EXPECT_EQ(n.room.stats().connections, std::size_t(0));
}

TEST_CASE(
    "A draft is shown to the others, coalesced, and locks after a pause") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.clear();
    n.say(1, "draft\t1\thi");
    n.say(1, "draft\t1\thi th");
    n.say(1, "draft\t1\thi there");
    n.advance(50);
    EXPECT_EQ(n.count(2, "draft\tada\t"), 1);
    EXPECT(n.last(2, "draft\tada\t") == "draft\tada\thi there");
    EXPECT(!n.got(1, "draft\t")); // The author does not get it back.
    n.say(1, "lock\t1");
    EXPECT(n.got(2, "msg\t1\tada\thi there"));
    EXPECT(n.got(1, "msg\t1\tada\thi there"));
    EXPECT(n.got(1, "ack\t1\t1\thi there"));
    EXPECT(!n.got(2, "ack"));
    n.advance(100);
    EXPECT(n.got(2, "gone\tada"));
}

TEST_CASE("The server locks a still bubble by itself, only once") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.say(1, "draft\t1\t  hello  ");
    n.advance(6000);
    EXPECT(!n.got(2, "msg"));
    n.advance(1000);
    EXPECT_EQ(n.count(2, "msg\t"), 1);
    EXPECT(n.got(2, "msg\t1\tada\thello")); // Trimmed.
    n.advance(20000);
    EXPECT_EQ(n.count(2, "msg\t"), 1);
}

TEST_CASE("Bubble numbers: stale and duplicate lines do nothing") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.say(1, "draft\t5\tfirst");
    n.say(1, "lock\t5");
    n.say(1, "lock\t5");         // Duplicate lock.
    n.say(1, "draft\t5\tagain"); // Late draft for a locked bubble.
    n.say(1, "draft\t4\told");   // Older bubble.
    n.advance(10000);
    EXPECT_EQ(n.count(2, "msg\t"), 1);
    // A draft with a new number starts a new bubble; an open one is locked
    // first.
    n.say(1, "draft\t6\tsecond");
    n.advance(2100);
    n.say(1, "draft\t7\tthird");
    EXPECT(n.got(2, "msg\t2\tada\tsecond"));
    n.say(1, "lock\t7");
    EXPECT(n.got(2, "msg\t3\tada\tthird"));
}

TEST_CASE("An empty bubble never locks and withdraws the draft") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.say(1, "draft\t1\toops");
    n.advance(400);
    EXPECT(n.got(2, "draft\tada\toops"));
    n.clear();
    n.say(1, "draft\t1\t");
    EXPECT(n.got(2, "gone\tada"));
    n.say(1, "lock\t1");
    n.say(1, "draft\t1\t   ");
    n.say(1, "lock\t1");
    n.advance(10000);
    EXPECT(!n.got(2, "msg"));
}

TEST_CASE("A message token bucket: burst, then slow down, then it refills") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    for (int i = 1; i <= 3; ++i) {
        n.say(1, "draft\t" + std::to_string(i) + "\tmsg " + std::to_string(i));
        n.say(1, "lock\t" + std::to_string(i));
        n.now += 100;
    }
    EXPECT_EQ(n.count(2, "msg\t"), 3);
    n.say(1, "draft\t4\tfourth");
    n.say(1, "lock\t4");
    EXPECT(n.got(1, "error\trate"));
    EXPECT_EQ(n.count(2, "msg\t"), 3);
    // The bubble stays open and goes out once a token is back.
    n.advance(2500);
    n.say(1, "lock\t4");
    EXPECT_EQ(n.count(2, "msg\t"), 4);
}

TEST_CASE("Connection caps: total, per source, without hello, locked, banned") {
    RoomConfig config;
    config.maxConnections = 4;
    config.maxPerSource = 2;
    config.maxUnhelloed = 3;
    Net n(config);
    n.connect(1, "a");
    n.connect(2, "a");
    n.connect(3, "a");
    EXPECT(n.closed.count(3) == 1); // Third from one source.
    n.connect(4, "b");
    EXPECT(n.closed.count(4) == 0);
    n.connect(5, "c");
    EXPECT(n.closed.count(5) == 1); // Three are still without hello.
    n.say(1, "hello\tada");
    n.connect(6, "d");
    EXPECT(n.closed.count(6) == 0);
    n.connect(7, "e");
    EXPECT(n.closed.count(7) == 1); // Four connections in all.
    std::string reply;
    n.apply(n.room.admin("lock", n.now, reply));
    n.apply(n.room.closed(6, n.now));
    n.connect(8, "f");
    EXPECT(n.closed.count(8) == 1);
    n.apply(n.room.admin("unlock", n.now, reply));
    n.connect(9, "f");
    EXPECT(n.closed.count(9) == 0);
}

TEST_CASE("The ring keeps the newest messages, drops the oldest and expires") {
    RoomConfig config;
    config.ringSlots = 5;
    config.ttl = 60 * 1000;
    config.messageBurst = 1000;
    config.messageRefill = 1;
    config.messagesPerMinute = 1000;
    Net n(config);
    n.join(1, "ada");
    for (int i = 1; i <= 8; ++i) {
        n.say(1,
              "draft\t" + std::to_string(i) + "\tmessage " + std::to_string(i));
        n.say(1, "lock\t" + std::to_string(i));
        n.now += 10;
    }
    EXPECT_EQ(n.room.stats().ring, std::size_t(5));
    n.join(2, "bo");
    EXPECT_EQ(n.count(2, "hist\t"), 5);
    EXPECT(n.got(2, "hist\t4\t0\tada\tmessage 4"));
    EXPECT(!n.got(2, "hist\t3\t"));
    n.clear();
    n.advance(61 * 1000);
    EXPECT_EQ(n.room.stats().ring, std::size_t(0));
    n.join(3, "cy");
    EXPECT_EQ(n.count(3, "hist\t"), 0);
}

TEST_CASE("Only the six most recent typers are shown, the rest are counted") {
    Net n;
    for (ConnId id = 1; id <= 9; ++id) {
        n.join(id, "user" + std::to_string(id));
    }
    n.join(10, "watcher");
    n.clear();
    for (ConnId id = 1; id <= 9; ++id) {
        n.say(id, "draft\t1\ttyping " + std::to_string(id));
        n.now += 10;
    }
    n.advance(500);
    int shown = 0;
    for (ConnId id = 1; id <= 9; ++id) {
        shown += n.got(10, "draft\tuser" + std::to_string(id) + "\t");
    }
    EXPECT_EQ(shown, 6);
    EXPECT(n.got(10, "typers\t3"));
    EXPECT(n.got(10, "draft\tuser9\t")); // The newest are in.
    EXPECT(!n.got(10, "draft\tuser1\t"));
    // One stops: the count falls and an older typer comes into view.
    n.say(9, "draft\t1\t");
    n.advance(500);
    EXPECT(n.got(10, "typers\t2"));
    EXPECT(n.got(10, "draft\tuser3\t"));
}

TEST_CASE("Mistakes add up to a kick and a short ban") {
    Net n;
    n.connect(1, "bad");
    for (int i = 0; i < 4; ++i) {
        n.say(1, "nonsense");
    }
    EXPECT(n.closed.count(1) == 0);
    n.say(1, "nonsense");
    EXPECT(n.closed.count(1) == 1);
    EXPECT(n.got(1, "error\tbanned"));
    n.connect(2, "bad");
    EXPECT(n.closed.count(2) == 1); // Banned for a while.
    n.advance(6 * 60 * 1000);
    n.connect(3, "bad");
    EXPECT(n.closed.count(3) == 0);
}

TEST_CASE("A draft flood and bad frames are not tolerated") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    for (int i = 0; i < 40; ++i) {
        n.say(1, "draft\t1\ta" + std::to_string(i));
    }
    EXPECT(n.closed.count(1) == 1);
    n.apply(n.room.badFrame(2, n.now));
    EXPECT(n.closed.count(2) == 1);
}

TEST_CASE("A reader that falls behind is dropped") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.apply(n.room.backlog(1, 1000, n.now));
    EXPECT(n.closed.count(1) == 0);
    n.apply(n.room.backlog(1, 10 * 1000 * 1000, n.now));
    EXPECT(n.closed.count(1) == 1);
    EXPECT(n.got(2, "leave\tada"));
}

TEST_CASE("Renaming: allowed, announced, not too often, not to a taken name") {
    Net n;
    n.join(1, "ada");
    n.join(2, "bo");
    n.clear();
    n.say(1, "nick\tbo");
    EXPECT(n.got(1, "error\tname-taken"));
    n.now += 11000;
    n.say(1, "nick\tadam");
    EXPECT(n.got(1, "welcome\tadam"));
    EXPECT(n.got(2, "leave\tada"));
    EXPECT(n.got(2, "join\tadam"));
    n.say(1, "nick\tadam2");
    EXPECT(n.got(1, "error\trate"));
}

TEST_CASE("Operator commands") {
    Net n;
    n.join(1, "ada", "1.1.1.1");
    n.join(2, "bo", "2.2.2.2");
    n.say(1, "draft\t1\tkeep me");
    n.say(1, "lock\t1");
    std::string reply;
    n.apply(n.room.admin("list", n.now, reply));
    EXPECT(reply.find("ada") != std::string::npos);
    EXPECT(reply.find("2.2.2.2") !=
           std::string::npos); // The source: here only.
    n.apply(n.room.admin("stats", n.now, reply));
    EXPECT(reply.find("online 2") != std::string::npos);
    n.clear();
    n.apply(n.room.admin("say the room closes at ten", n.now, reply));
    EXPECT(n.got(1, "notice\tthe room closes at ten"));
    n.apply(n.room.admin("say bad \x1b text", n.now, reply));
    EXPECT(reply.find("not allowed") != std::string::npos);
    n.apply(n.room.admin("delete 1", n.now, reply));
    EXPECT(n.got(2, "hide\t1"));
    n.apply(n.room.admin("delete 99", n.now, reply));
    EXPECT(reply.find("no such") != std::string::npos);
    // Mute: the draft goes nowhere and nothing is posted.
    n.apply(n.room.admin("mute bo 5", n.now, reply));
    n.clear();
    n.say(2, "draft\t1\tshh");
    n.say(2, "lock\t1");
    n.advance(1000);
    EXPECT(!n.got(1, "draft"));
    EXPECT(!n.got(1, "msg"));
    n.apply(n.room.admin("unmute bo", n.now, reply));
    // Kick, then ban by name closes the connection and refuses a new one.
    n.apply(n.room.admin("ban bo 30", n.now, reply));
    EXPECT(n.closed.count(2) == 1);
    n.connect(3, "2.2.2.2");
    EXPECT(n.closed.count(3) == 1);
    n.apply(n.room.admin("unban 2.2.2.2", n.now, reply));
    n.connect(4, "2.2.2.2");
    EXPECT(n.closed.count(4) == 0);
    n.apply(n.room.admin("kick ada", n.now, reply));
    EXPECT(n.closed.count(1) == 1);
    n.apply(n.room.admin("kick nobody", n.now, reply));
    EXPECT(reply.find("no such") != std::string::npos);
    n.apply(n.room.admin("frobnicate", n.now, reply));
    EXPECT(reply.find("unknown") != std::string::npos);
}

TEST_CASE("Nothing pending means no deadline") {
    Net n;
    EXPECT(!n.room.nextDeadline().has_value());
    n.join(1, "ada");
    n.advance(2000);
    EXPECT(!n.room.nextDeadline().has_value());
    n.say(1, "draft\t1\thi");
    EXPECT(n.room.nextDeadline().has_value());
}

TEST_CASE("Fuzz: any sequence of events keeps every promise") {
    Lcg rng{99};
    const char *frames[] = {
        "hello\tada",        "hello\tbo",     "hello\tcy",
        "nick\tdee",         "draft\t1\thi",  "draft\t2\tthere",
        "draft\t3\t",        "draft\t3\t x ", "lock\t1",
        "lock\t2",           "lock\t3",       "lock\t9",
        "draft\t1\t\x1b[2J", "hello",         "",
        "draft\t0\tx",       "garbage",       "draft\t4\tfour",
        "lock\t4",           "nick\tada",     "draft\t999999999\tbig"};
    for (int run = 0; run < 60; ++run) {
        RoomConfig config;
        config.ringSlots = 1 + rng.next() % 8;
        config.maxConnections = 2 + rng.next() % 6;
        config.ttl = 5000 + rng.next() % 20000;
        Net n(config);
        std::set<ConnId> open;
        for (int step = 0; step < 400; ++step) {
            const auto id = ConnId(1 + rng.next() % 10);
            switch (rng.next() % 9) {
            case 0:
                n.connect(id, "s" + std::to_string(rng.next() % 4));
                break;
            case 1:
                n.apply(n.room.closed(id, n.now));
                break;
            case 2:
                n.apply(n.room.badFrame(id, n.now));
                break;
            case 3:
                n.advance(Millis(rng.next() % 3000));
                break;
            case 4: {
                std::string reply;
                const char *cmds[] = {"list",
                                      "stats",
                                      "clear",
                                      "lock",
                                      "unlock",
                                      "mute ada 1",
                                      "ban ada 1",
                                      "say hello",
                                      "delete 1",
                                      "kick bo"};
                n.apply(n.room.admin(cmds[rng.next() % 10], n.now, reply));
                break;
            }
            default:
                n.say(id,
                      frames[rng.next() % (sizeof frames / sizeof *frames)]);
                break;
            }
            n.now += Millis(rng.next() % 200);
            const auto stats = n.room.stats();
            EXPECT(stats.connections <= config.maxConnections);
            EXPECT(stats.ring <= config.ringSlots);
        }
        n.advance(60000);
        // Every message that went out passed the text rules.
        for (const auto &[id, lines] : n.mail) {
            (void)id;
            for (const auto &l : lines) {
                const auto p = parseServerLine(l);
                EXPECT(p.error == ProtoError::None);
                if (p.line.kind == ServerLine::Kind::Msg) {
                    EXPECT(checkMessage(p.line.text).ok());
                }
            }
        }
    }
}

TEST_SUIT_END
