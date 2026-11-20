#include "chatclient.h"
#include "chatproto.h"
#include "chatroom.h"
#include "chattext.h"
#include "mls-unit-test/unittest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace chat;
using Client = ChatClient;

Client::Config named(const std::string &name) {
    Client::Config c;
    c.name = name;
    return c;
}

// A client that is already in the room: connected, hello answered, ready.
struct Scripted {
    explicit Scripted(const std::string &name = "ada")
        : client(named(name)) {
        client.connect(now);
        absorb(client.opened(now));
        feed("welcome\t" + name);
        feed("users\t" + name);
        feed("ready");
        feed("online\t1");
        client.takeEvents();
        sent.clear();
    }
    void absorb(const Client::Out &out) {
        for (const auto &l : out.send) {
            sent.push_back(l);
        }
        closeRequested = closeRequested || out.close;
    }
    void feed(const std::string &frame) {
        absorb(client.receive(frame, now));
    }
    void edit(const std::string &text) {
        Client::Out out;
        EXPECT(client.edit(text, now, out));
        absorb(out);
    }
    void advance(Millis ms) {
        const Millis end = now + ms;
        while (now < end) {
            now = std::min<Millis>(end, now + 50);
            absorb(client.tick(now));
        }
    }
    int count(const std::string &prefix) const {
        int n = 0;
        for (const auto &l : sent) {
            n += l.compare(0, prefix.size(), prefix) == 0;
        }
        return n;
    }
    Client client;
    Millis now = 1000;
    std::vector<std::string> sent;
    bool closeRequested = false;
};

// Clients and a room joined by plain function calls.
struct World {
    ChatRoom room;
    Millis now = 1000;
    std::map<ConnId, std::unique_ptr<Client>> clients;
    void deliver(ConnId from, const Client::Out &out) {
        for (const auto &line : out.send) {
            route(room.line(from, line, now));
        }
        if (out.close) {
            route(room.closed(from, now));
        }
    }
    void route(const Actions &actions) {
        for (const auto &a : actions) {
            const auto it = clients.find(a.conn);
            if (it == clients.end()) {
                continue;
            }
            if (a.close) {
                it->second->closed(now);
            }
            else {
                deliver(a.conn, it->second->receive(a.line, now));
            }
        }
    }
    Client &join(ConnId id, const std::string &name) {
        clients[id] = std::make_unique<Client>(named(name));
        Client &c = *clients[id];
        c.connect(now);
        route(room.connected(id, "src" + std::to_string(id), now));
        deliver(id, c.opened(now));
        return c;
    }
    void edit(ConnId id, const std::string &text) {
        Client::Out out;
        EXPECT(clients.at(id)->edit(text, now, out));
        deliver(id, out);
    }
    void advance(Millis ms) {
        const Millis end = now + ms;
        while (now < end) {
            now = std::min<Millis>(end, now + 50);
            route(room.tick(now));
            for (auto &[id, c] : clients) {
                deliver(id, c->tick(now));
            }
        }
    }
};

bool clean(const std::string &s) {
    std::u32string cps;
    if (!decodeUtf8Strict(s, cps)) {
        return false;
    }
    for (const char32_t c : cps) {
        if (isControlOrFormat(c) || !(isAllowedChar(c) || c == 0xFFFD)) {
            return false;
        }
    }
    return true;
}

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return std::uint32_t(s >> 33);
    }
};
} // namespace

TEST_SUIT_BEGIN(ChatClient)

TEST_CASE("Connecting: nothing is sent before the person asks, then hello") {
    Client c(named("ada"));
    EXPECT(c.state() == Client::State::Off);
    EXPECT(c.tick(5000).send.empty());
    EXPECT(c.receive("ready", 0).send.empty()); // Ignored while off.
    c.connect(0);
    EXPECT(c.state() == Client::State::Connecting);
    const auto out = c.opened(0);
    EXPECT(out.send.size() == std::size_t(1));
    EXPECT(out.send[0] == "hello\tada");
    EXPECT(c.state() == Client::State::Hello);
}

TEST_CASE("Welcome, users, history and ready make the room appear") {
    Client c(named("ada"));
    c.connect(0);
    c.opened(0);
    c.receive("welcome\tada", 0);
    c.receive("users\tada,bo", 0);
    c.receive("hist\t7\t90\tbo\tearlier message", 100000);
    c.receive("ready", 100000);
    c.receive("online\t2", 100000);
    EXPECT(c.state() == Client::State::Joined);
    EXPECT(c.users().size() == std::size_t(2));
    EXPECT(c.online() == 2u);
    EXPECT(c.history().size() == std::size_t(1));
    EXPECT(c.history()[0].text == "earlier message");
    EXPECT(c.history()[0].at == 100000 - 90000);
    const auto events = c.takeEvents();
    int messages = 0;
    for (const auto &e : events) {
        messages += e.kind == Client::Event::Kind::Message;
    }
    EXPECT(messages == 1);
    EXPECT(c.takeEvents().empty());
}

TEST_CASE("A name that is taken asks for another one") {
    Client c(named("ada"));
    c.connect(0);
    c.opened(0);
    c.receive("error\tname-taken\tthat name is in use", 0);
    EXPECT(c.state() == Client::State::NeedName);
    EXPECT(c.hello("bad name", 0).send.empty()); // Not a valid name: nothing.
    const auto out = c.hello("ada2", 0);
    EXPECT(out.send.size() == std::size_t(1));
    EXPECT(out.send[0] == "hello\tada2");
    EXPECT(c.state() == Client::State::Hello);
}

TEST_CASE("Typing sends the whole draft, at most about three times a second") {
    Scripted s;
    s.edit("h");
    EXPECT(s.sent.back() == "draft\t1\th");
    s.edit("hi");
    s.edit("hi t");
    s.edit("hi th");
    EXPECT_EQ(s.count("draft"), 1); // Held back for now.
    s.advance(400);
    EXPECT_EQ(s.count("draft"), 2);
    EXPECT(s.sent.back() == "draft\t1\thi th"); // The latest, in full.
    s.advance(300);
    EXPECT_EQ(s.count("draft"), 2); // Nothing changed: nothing sent.
    s.edit("");
    EXPECT(s.sent.back() == "draft\t1\t"); // Withdrawn.
    EXPECT(s.client.ownState() == Client::OwnState::Idle);
}

TEST_CASE("Characters the room refuses cannot be typed") {
    Scripted s;
    Client::Out out;
    EXPECT(!s.client.edit("\x1b[31m", s.now, out));
    EXPECT(!s.client.edit("smile \xF0\x9F\x98\x80", s.now, out));
    EXPECT(!s.client.edit(std::string(201, 'a'), s.now, out));
    EXPECT(!s.client.edit("aaaaaaaaaaaa", s.now, out));
    EXPECT(s.client.draft().empty());
    EXPECT(s.client.edit("  hello  ", s.now, out)); // Edge spaces are fine.
}

TEST_CASE("After five still seconds the draft is sent and locked") {
    Scripted s;
    s.edit("hello there");
    s.advance(4800);
    EXPECT_EQ(s.count("lock"), 0);
    s.advance(400);
    EXPECT_EQ(s.count("lock"), 1);
    EXPECT(s.sent.back() == "lock\t1");
    s.advance(10000);
    EXPECT_EQ(s.count("lock"), 1); // Once.
    // Every edit restarts the wait.
    Scripted t;
    t.edit("a");
    t.advance(4000);
    t.edit("ab");
    t.advance(4000);
    EXPECT_EQ(t.count("lock"), 0);
}

TEST_CASE("Enter locks at once") {
    Scripted s;
    s.edit("quick");
    s.absorb(s.client.submit(s.now));
    EXPECT(s.sent.back() == "lock\t1");
    Scripted empty;
    empty.absorb(empty.client.submit(empty.now));
    EXPECT(empty.sent.empty()); // Nothing to lock.
}

TEST_CASE(
    "The acknowledgement is cut from the start; text typed meanwhile stays") {
    Scripted s;
    s.edit("hello");
    s.absorb(s.client.submit(s.now));
    s.edit("hello wor"); // Typed before the answer arrived.
    s.feed("msg\t5\tada\thello");
    s.feed("ack\t1\t5\thello");
    EXPECT(s.client.draft() == "wor");
    EXPECT(s.client.ownBubble() == 2u);
    EXPECT(s.sent.back() ==
           "draft\t2\twor"); // The rest goes on as a new bubble.
    // Nothing typed meanwhile: the input is empty and a new bubble is ready.
    Scripted t;
    t.edit("done");
    t.absorb(t.client.submit(t.now));
    t.feed("ack\t1\t9\tdone");
    EXPECT(t.client.draft().empty());
    EXPECT(t.client.ownBubble() == 2u);
    // Edited inside the posted text meanwhile: nothing is lost.
    Scripted u;
    u.edit("hello");
    u.absorb(u.client.submit(u.now));
    u.edit("jello");
    u.feed("ack\t1\t3\thello");
    EXPECT(u.client.draft() == "jello");
    // An acknowledgement for another bubble does nothing.
    Scripted v;
    v.edit("keep");
    v.feed("ack\t9\t3\tkeep");
    EXPECT(v.client.draft() == "keep");
}

TEST_CASE(
    "Not sent: a lost connection keeps the text; connecting again sends it") {
    Scripted s;
    s.edit("unsent thought");
    s.absorb(s.client.leave(s.now));
    EXPECT(s.client.ownState() == Client::OwnState::NotSent);
    s.sent.clear();
    s.client.connect(s.now);
    s.absorb(s.client.opened(s.now));
    s.feed("welcome\tada");
    s.feed("users\tada");
    s.feed("ready");
    EXPECT(s.sent.back() == "draft\t1\tunsent thought");
    EXPECT(s.client.ownState() == Client::OwnState::DraftSent);
    // The same when the server closes the connection.
    Scripted t;
    t.edit("another");
    t.absorb(t.client.closed(t.now));
    EXPECT(t.client.ownState() == Client::OwnState::NotSent);
    EXPECT(t.client.state() == Client::State::Closed);
}

TEST_CASE(
    "Others: live drafts, locked messages, joins, leaves, hide, notices") {
    Scripted s;
    s.feed("join\tbo");
    s.feed("draft\tbo\tthinking about");
    EXPECT(s.client.typers().size() == std::size_t(1));
    EXPECT(s.client.typers()[0].text == "thinking about");
    s.feed("draft\tbo\tthinking about it");
    EXPECT(s.client.typers().size() == std::size_t(1));
    s.feed("msg\t3\tbo\tthinking about it");
    EXPECT(s.client.typers().empty());
    EXPECT(s.client.history().back().text == "thinking about it");
    EXPECT(!s.client.history().back().own);
    s.feed("typers\t2");
    EXPECT(s.client.hiddenTypers() == 2u);
    s.feed("hide\t3");
    EXPECT(s.client.history().empty());
    s.feed("notice\tthe room closes at ten");
    s.feed("leave\tbo");
    EXPECT(s.client.users().size() == std::size_t(1));
    const auto events = s.client.takeEvents();
    EXPECT(events.size() >= std::size_t(5));
}

TEST_CASE(
    "An error that closes the door closes it; a rate error retries the lock") {
    Scripted s;
    s.feed("error\tbanned\tgo away");
    EXPECT(s.closeRequested);
    EXPECT(s.client.state() == Client::State::Closed);
    EXPECT(s.client.closeReason() == "go away");
    Scripted t;
    t.edit("hello");
    t.advance(5200);
    EXPECT_EQ(t.count("lock"), 1);
    t.feed("error\trate\tslow down");
    EXPECT(t.client.state() == Client::State::Joined);
    t.advance(5200);
    EXPECT_EQ(t.count("lock"), 2);
}

TEST_CASE("Hostile server lines never get through unsafe") {
    Scripted s;
    s.feed("msg\t1\tbo\t\x1b]52;c;AAAA\x07 x"); // Control bytes: dropped.
    s.feed("notice\t\x1b[2Jhello");
    s.feed("msg\t2\tbo\t\xC2\x9B"
           "31m\xE2\x80\xAE"
           "evil");                     // Sanitized.
    s.feed("msg\t3\tserver\timpostor"); // Reserved name: dropped.
    s.feed("bogus line");
    EXPECT(s.client.history().size() == std::size_t(1));
    EXPECT(s.client.history()[0].text == "31mevil");
    for (const auto &b : s.client.history()) {
        EXPECT(clean(b.text));
    }
    // A server that keeps talking nonsense is dropped.
    for (int i = 0; i < 30; ++i) {
        s.feed("nonsense");
    }
    EXPECT(s.closeRequested);
    EXPECT(s.client.state() == Client::State::Closed);
}

TEST_CASE(
    "Two clients and a room: live typing, locking, history for a latecomer") {
    World w;
    Client &ada = w.join(1, "ada");
    Client &bo = w.join(2, "bo");
    w.advance(100);
    EXPECT(ada.state() == Client::State::Joined);
    EXPECT(bo.users().size() == std::size_t(2));
    w.edit(1, "anyone seen the crumble game?");
    w.advance(500);
    EXPECT(bo.typers().size() == std::size_t(1));
    EXPECT(bo.typers()[0].text == "anyone seen the crumble game?");
    EXPECT(ada.typers().empty());
    w.advance(5200); // Still: the client locks.
    EXPECT(bo.typers().empty());
    EXPECT(bo.history().size() == std::size_t(1));
    EXPECT(bo.history()[0].name == "ada");
    EXPECT(ada.history().size() == std::size_t(1));
    EXPECT(ada.history()[0].own);
    EXPECT(ada.draft().empty());
    EXPECT(ada.ownBubble() == 2u);
    w.edit(2, "it is hidden in /usr/games");
    w.advance(7000);
    Client &cy = w.join(3, "cy");
    w.advance(100);
    EXPECT(cy.history().size() == std::size_t(2)); // The late one catches up.
    EXPECT(cy.history()[1].text == "it is hidden in /usr/games");
    // Leaving is seen.
    w.deliver(2, bo.leave(w.now));
    w.advance(1500);
    EXPECT(ada.users().size() == std::size_t(2));
    EXPECT(ada.online() == 2u);
}

TEST_CASE("The server may lock first: nothing typed is lost") {
    World w;
    Client &ada = w.join(1, "ada");
    Client &bo = w.join(2, "bo");
    (void)bo;
    w.edit(1, "hello");
    // Simulate a slow client: stop its own timer, let the server's backstop
    // act.
    w.now += 6600;
    w.route(w.room.tick(w.now));
    EXPECT(ada.history().size() == std::size_t(1));
    EXPECT(ada.draft().empty());
    EXPECT(ada.ownBubble() == 2u);
}

TEST_CASE("Fuzz: whatever the server sends, the model stays safe and bounded") {
    Lcg rng{31337};
    const char *pieces[] = {"msg",
                            "draft",
                            "hist",
                            "users",
                            "notice",
                            "error",
                            "join",
                            "leave",
                            "gone",
                            "hide",
                            "ack",
                            "online",
                            "typers",
                            "welcome",
                            "ready",
                            "\t",
                            "\t",
                            "\t",
                            "1",
                            "7",
                            "ada",
                            "bo",
                            "hello",
                            "\x1b",
                            "\xC2\x9B",
                            "\xE2\x80\xAE",
                            "\xF0\x9F\x98\x80",
                            "\xFF",
                            "banned",
                            "rate",
                            "name-taken",
                            ",",
                            " "};
    for (int run = 0; run < 200; ++run) {
        Client::Config config = named("ada");
        config.maxHistory = 10;
        config.maxEvents = 20;
        config.maxUsers = 8;
        Client c(config);
        Millis now = 0;
        c.connect(now);
        c.opened(now);
        for (int step = 0; step < 150; ++step) {
            now += Millis(rng.next() % 500);
            std::string frame;
            const int n = 1 + int(rng.next() % 7);
            for (int i = 0; i < n; ++i) {
                frame += pieces[rng.next() % (sizeof pieces / sizeof *pieces)];
            }
            switch (rng.next() % 7) {
            case 0: {
                Client::Out out;
                c.edit(
                    std::string(1 + rng.next() % 5, char('a' + rng.next() % 3)),
                    now,
                    out);
                break;
            }
            case 1:
                c.tick(now);
                break;
            case 2:
                c.submit(now);
                break;
            case 3:
                c.closed(now);
                c.connect(now);
                c.opened(now);
                break;
            default:
                c.receive(frame, now);
                break;
            }
            EXPECT(c.history().size() <= std::size_t(10));
            EXPECT(c.users().size() <= std::size_t(8));
            EXPECT(c.typers().size() <= std::size_t(16));
            EXPECT(checkDraft(c.draft()).ok());
            for (const auto &b : c.history()) {
                EXPECT(clean(b.text));
            }
            for (const auto &t : c.typers()) {
                EXPECT(clean(t.text));
            }
        }
        for (const auto &e : c.takeEvents()) {
            EXPECT(clean(e.text));
            EXPECT(clean(e.bubble.text));
        }
    }
}

TEST_SUIT_END
