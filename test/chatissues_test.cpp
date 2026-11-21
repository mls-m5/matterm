// Gaps found in the chat's security review (docs/CHAT_REVIEW.md). Each suite
// states what SHOULD hold and fails today; CTest registers each one as an
// expected failure (WILL_FAIL), so the suite stays green until the code is
// fixed and then turns red as a reminder to move the check into the passing
// tests.
#include "chatdfixture.h"
#include "chatproto.h"
#include "chatroom.h"
#include "mls-unit-test/unittest.h"

#include <string>
#include <vector>

namespace {
using namespace chat;

bool hasLine(const Actions &actions, ConnId id, const std::string &prefix) {
    for (const auto &a : actions) {
        if (a.conn == id && !a.close &&
            a.line.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

// 200 allowed characters, different for every i, with no long runs.
std::string longText(int i) {
    std::string text = "m" + std::to_string(i) + " ";
    const std::string pattern = "abcdefghijklmnopqrstuvwxyz";
    for (std::size_t k = 0; text.size() < 200; ++k) {
        text += pattern[(std::size_t(i) + k) % pattern.size()];
    }
    return text;
}
} // namespace

// ISSUE 1. A newcomer must be able to join a room whose history is full.
// The adapter queues the whole history (200 lines of up to ~230 bytes) before
// the first write completes, and the room drops a connection whose queue passes
// maxBacklogBytes (16 KiB). About 70 long messages are enough to make every
// later join fail until they expire (45 minutes): a cheap, lasting denial of
// service.
TEST_SUIT_BEGIN(ChatIssueJoinBurst)
TEST_CASE("A full history does not keep newcomers out") {
    chatd::Options o = quick();
    o.room.messageBurst = 1000;
    o.room.messageRefill = 1;
    o.room.messagesPerMinute = 100000;
    o.room.draftFloodPerSecond = 100000;
    Running r(o);
    Line ada(r.server->tcpPort(), 5);
    ada.send("hello\tada");
    EXPECT(ada.expect("ready"));
    for (int i = 1; i <= 100; ++i) {
        ada.send("draft\t" + std::to_string(i) + "\t" + longText(i));
        ada.send("lock\t" + std::to_string(i));
    }
    EXPECT(ada.expect("msg\t100\t"));
    Line cy(r.server->tcpPort(), 5);
    cy.send("hello\tcy");
    EXPECT(cy.expect("ready"));
}
TEST_SUIT_END

// ISSUE 2. One source must not be able to use up the whole budget for
// connections that have not finished the handshake (maxPending, 128), because
// the per-source caps only start after the handshake. A single address that
// keeps 128 sockets silent (each is dropped after 5 s, then it opens more)
// refuses everyone else. A reverse proxy normally absorbs this, but chatd is
// the last line if it is reachable directly.
TEST_SUIT_BEGIN(ChatIssuePendingBudget)
TEST_CASE("Silent connections from one address do not shut others out") {
    chatd::Options o = quick();
    o.maxPending = 8;
    Running r(o);
    net::io_context io;
    std::vector<tcp::socket> silent;
    for (int i = 0; i < 8; ++i) {
        silent.emplace_back(io);
        silent.back().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"),
                                            r.server->wsPort()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Another address (127.0.0.2) asks politely.
    WsClient other(r.server->wsPort(), kOrigin, {}, "127.0.0.2");
    EXPECT(other.handshake());
}
TEST_SUIT_END

// ISSUE 3. Names that look like another name or a reserved one must not get in.
// Names are plain ASCII (good), but I/l/1, O/0 and rn/m are still look-alikes:
// "Iike" next to "like", "adm1n" or "adrnin" next to the reserved "admin", and
// the reserved list is the defence against impersonating the operator.
TEST_SUIT_BEGIN(ChatIssueLookAlikeNames)
TEST_CASE("Look-alikes of a taken or reserved name are refused") {
    ChatRoom room;
    Millis now = 0;
    ConnId next = 1;
    auto join = [&](const std::string &name) {
        const ConnId id = next++;
        room.connected(id, "source" + std::to_string(id), now);
        return hasLine(room.line(id, "hello\t" + name, now), id, "welcome");
    };
    EXPECT(join("like"));
    EXPECT(join("bob"));
    EXPECT(!join("Iike")); // Capital i for l.
    EXPECT(!join("1ike"));
    EXPECT(!join("b0b"));
    EXPECT(!join("bOb"));
    for (const char *reserved : {"adm1n",
                                 "admln",
                                 "adrnin",
                                 "Admln",
                                 "serv3r",
                                 "syst3m",
                                 "r00t",
                                 "rn0derator",
                                 "y0u",
                                 "m0d"}) {
        EXPECT(!join(reserved));
    }
}
TEST_SUIT_END

// ISSUE 4. The room-wide cap on messages per minute must not let a few
// connections lock everyone else out of posting, and must not get the others
// banned. Five connections on two addresses, each sending one message every two
// seconds (the per-connection rate, staggered), reach 150 per minute, above the
// room's 120. A person on a third address who tries to post is told "slow down"
// every time, and since each refused lock counts as that person's own mistake,
// five tries get them kicked and banned for five minutes. The attackers never
// send an explicit lock (the server locks for them), so nothing counts against
// them. (Measured: 0 of 12 messages got through; the newcomer was kicked.)
TEST_SUIT_BEGIN(ChatIssueRoomBudget)
TEST_CASE("A few spammers cannot take the whole message budget or get others "
          "banned") {
    ChatRoom room;
    Millis now = 0;
    struct Attacker {
        ConnId id;
        std::uint32_t bubble = 0;
        Millis offset = 0;
    };
    std::vector<Attacker> attackers;
    ConnId next = 1;
    for (int i = 0; i < 5; ++i) {
        const ConnId id = next++;
        room.connected(id, i < 3 ? "address-a" : "address-b", now);
        room.line(id, "hello\tspam" + std::to_string(i), now);
        attackers.push_back({id, 0, Millis(i) * 400});
    }
    const ConnId victim = next++;
    room.connected(victim, "address-c", now);
    room.line(victim, "hello\tvictim", now);
    int delivered = 0;
    std::uint32_t bubble = 0;
    for (; now < 120000; now += 50) {
        for (auto &a : attackers) {
            if (now >= a.offset && (now - a.offset) % 2000 == 0) {
                room.line(a.id,
                          "draft\t" + std::to_string(++a.bubble) + "\tspam " +
                              std::to_string(now) + " " + std::to_string(a.id),
                          now);
            }
        }
        room.tick(now);
        if (now >= 60000 && now % 5000 == 0 && now < 90000) {
            ++bubble;
            room.line(victim,
                      "draft\t" + std::to_string(bubble) + "\thello " +
                          std::to_string(bubble),
                      now);
            const auto reply =
                room.line(victim, "lock\t" + std::to_string(bubble), now + 1);
            delivered += hasLine(reply, victim, "ack\t");
        }
    }
    EXPECT(delivered >= 1);
    EXPECT_EQ(room.stats().connections, std::size_t(6)); // Nobody was kicked.
}
TEST_SUIT_END

// ISSUE 5. The table of bans must be bounded. A kick after repeated mistakes
// bans the source for five minutes and nothing limits how many sources can be
// banned: an attacker with many addresses (an IPv6 /48 has 65536 /64 keys)
// grows the table without bound at the cost of five bad lines each.
TEST_SUIT_BEGIN(ChatIssueBanTable)
TEST_CASE("Banning many sources does not grow memory without bound") {
    ChatRoom room;
    Millis now = 0;
    for (ConnId id = 1; id <= 6000; ++id, ++now) {
        room.connected(id, "source-" + std::to_string(id), now);
        for (int i = 0; i < 5; ++i) {
            room.line(id, "not a command", now);
        }
    }
    EXPECT(room.stats().bans <= 4096);
}
TEST_SUIT_END

// ISSUE 6. The rate of new connections from one source must be limited. Only
// concurrent connections are capped, so one address can connect, say hello and
// leave in a loop. Each round sends the newcomer the whole history (up to
// 46 KB) and every other person a join and a leave line plus a changed count:
// a small request that costs the server and 64 clients a great deal.
TEST_SUIT_BEGIN(ChatIssueReconnectChurn)
TEST_CASE("One address cannot join and leave hundreds of times a second") {
    ChatRoom room;
    Millis now = 0;
    for (int i = 0; i < 64 - 3; ++i) { // A busy room.
        const ConnId id = 100000 + ConnId(i);
        room.connected(id, "visitor-" + std::to_string(i), now);
        room.line(id, "hello\tuser" + std::to_string(i), now);
    }
    int admitted = 0;
    std::size_t actionsCaused = 0;
    for (ConnId id = 1; id <= 1000; ++id, now += 10) { // 100 per second.
        auto a = room.connected(id, "one-address", now);
        auto b = room.line(id, "hello\tchurn", now);
        actionsCaused += a.size() + b.size();
        if (hasLine(b, id, "welcome")) {
            ++admitted;
        }
        room.closed(id, now);
    }
    EXPECT(admitted <= 100);
    (void)actionsCaused;
}
TEST_SUIT_END
