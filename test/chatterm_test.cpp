#include "chatserver.h"
#include "mls-unit-test/unittest.h"
#include "ptylink.h"

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// mattrix-chat in real pseudo-terminals against a real chatd on loopback: two
// people typing at each other, as in two terminal windows.
namespace {
namespace net = boost::asio;

struct Server {
    Server() {
        chatd::Options o;
        o.wsEnabled = false;
        o.tcpEnabled = true;
        o.room.maxPerSource = 8;
        o.room.lockBackstop =
            60 * 1000; // The client's own timer is the one under test.
        server = std::make_unique<chatd::ChatServer>(io, o);
        server->start();
        thread = std::thread([this] { io.run(); });
    }
    ~Server() {
        net::post(io, [this] { server->stop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        io.stop();
        thread.join();
    }
    net::io_context io;
    std::unique_ptr<chatd::ChatServer> server;
    std::thread thread;
};

struct Person {
    Person(std::uint16_t port,
           const std::string &name,
           const std::string &term = "xterm-256color",
           PtyProcess::ColorCapability color =
               PtyProcess::ColorCapability::TrueColor)
        : link({CHAT_BINARY,
                "--tcp",
                "127.0.0.1:" + std::to_string(port),
                "--name",
                name,
                "--yes",
                "--no-animations"},
               80,
               24,
               term,
               color) {
        link.setEvents(
            {[this](std::string_view bytes) { output.append(bytes); },
             [this](int code, int) {
                 exited = true;
                 exitCode = code;
             },
             {}});
    }
    void pump() {
        link.pump();
    }
    PtyLink link;
    std::string output;
    std::size_t from = 0;
    bool exited = false;
    int exitCode = -2;
};

bool until(std::vector<Person *> people,
           Person &watch,
           const std::string &needle,
           int ms = 4000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto *p : people) {
            p->pump();
        }
        if (watch.output.find(needle, watch.from) != std::string::npos) {
            watch.from = watch.output.find(needle, watch.from) + needle.size();
            return true;
        }
        if (watch.exited) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool untilExit(Person &p, int ms = 4000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!p.exited && std::chrono::steady_clock::now() < deadline) {
        p.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p.exited;
}
} // namespace

TEST_SUIT_BEGIN(ChatTerminal)

TEST_CASE("Two people type at each other; drafts show live and Enter sends") {
    Server server;
    const auto port = server.server->tcpPort();
    Person ada(port, "ada");
    EXPECT(until({&ada}, ada, "connected as ada"));
    Person bo(port, "bo");
    std::vector<Person *> both{&ada, &bo};
    EXPECT(until(both, bo, "connected as bo"));
    EXPECT(until(both, ada, "bo joined"));
    ada.link.sendInput("hello bo");
    EXPECT(until(both, bo, "hello bo")); // Seen as a live draft.
    EXPECT(until(both, bo, "typing"));
    ada.link.sendInput("\r");
    EXPECT(until(both, bo, "sent")); // The locked bubble.
    bo.link.sendInput("hi ada");
    EXPECT(until(both, ada, "hi ada"));
    bo.link.sendInput("\r");
    EXPECT(until(both, ada, "sent"));
    // Ctrl-C leaves, and the other one sees it.
    ada.link.sendInput("\x03");
    EXPECT(untilExit(ada));
    EXPECT_EQ(ada.exitCode, 0);
    EXPECT(until(both, bo, "ada left"));
    EXPECT(bo.output.find("\x1b[?25h") !=
           std::string::npos); // The cursor is shown.
    bo.link.sendInput("\x03");
    EXPECT(untilExit(bo));
}

TEST_CASE("A pause locks the bubble by itself") {
    Server server;
    const auto port = server.server->tcpPort();
    Person ada(port, "ada");
    Person bo(port, "bo");
    std::vector<Person *> both{&ada, &bo};
    EXPECT(until(both, ada, "connected as ada"));
    EXPECT(until(both, bo, "connected as bo"));
    ada.link.sendInput("no enter needed");
    EXPECT(until(both, bo, "no enter needed"));
    // About five quiet seconds later it is posted.
    EXPECT(until(both, bo, "sent", 8000));
    ada.link.sendInput("\x03");
    bo.link.sendInput("\x03");
    EXPECT(untilExit(ada));
    EXPECT(untilExit(bo));
}

TEST_CASE("Without color or a Unicode locale the chat still works") {
    Server server;
    const auto port = server.server->tcpPort();
    Person plain(
        port, "ada", "linux", PtyProcess::ColorCapability::Unspecified);
    EXPECT(until({&plain}, plain, "connected as ada"));
    EXPECT(plain.output.find("38;2;") == std::string::npos);
    EXPECT(plain.output.find("38;5;") == std::string::npos);
    plain.link.sendInput("\x03");
    EXPECT(untilExit(plain));
}

TEST_CASE("Commands are not sent as drafts; a taken name asks for another") {
    Server server;
    const auto port = server.server->tcpPort();
    Person ada(port, "ada");
    Person bo(port, "bo");
    std::vector<Person *> both{&ada, &bo};
    EXPECT(until(both, ada, "connected as ada"));
    EXPECT(until(both, bo, "connected as bo"));
    ada.link.sendInput("/who\r");
    EXPECT(until(both, ada, "here: "));
    ada.link.sendInput("/nick secret");
    ada.link.sendInput("\r");
    EXPECT(until(both, bo, "ada left"));
    EXPECT(bo.output.find("/nick") == std::string::npos); // Never seen live.
    Person twin(port, "bo");
    EXPECT(until({&ada, &bo, &twin}, twin, "name taken"));
    twin.link.sendInput("bo2\r");
    EXPECT(until({&ada, &bo, &twin}, twin, "connected as bo2"));
    for (auto *p : {&ada, &bo, &twin}) {
        p->link.sendInput("\x03");
        EXPECT(untilExit(*p));
    }
}

TEST_CASE("A refused connection exits with an error") {
    PtyLink link({CHAT_BINARY, "--tcp", "127.0.0.1:1", "--yes"},
                 80,
                 24,
                 "xterm",
                 PtyProcess::ColorCapability::Unspecified);
    std::string output;
    bool exited = false;
    int code = -2;
    link.setEvents({[&](std::string_view b) { output.append(b); },
                    [&](int c, int) {
                        exited = true;
                        code = c;
                    },
                    {}});
    for (int i = 0; i < 2000 && !exited; ++i) {
        link.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT(exited);
    EXPECT_EQ(code, 1);
}

TEST_SUIT_END
