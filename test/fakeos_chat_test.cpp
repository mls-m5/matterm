#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"
#include "scriptedprogram.h"

#include <memory>
#include <string>
#include <vector>

using fakeos_test::Pc;

namespace {
// A host that provides the chat link and records what the program asks of it.
struct Host {
    std::vector<std::string> sent;
    int opened = 0, closed = 0;
};

fakeos::Config withChat(Host &host, bool enabled = true) {
    fakeos::Config c;
    c.seed = 7;
    c.intro = false;
    if (enabled) {
        c.chat.server = "chat.example.org";
        c.chat.open = [&host] { ++host.opened; };
        c.chat.send = [&host](const std::string &line) {
            host.sent.push_back(line);
        };
        c.chat.close = [&host] { ++host.closed; };
    }
    return c;
}

const std::string kPrompt = "guest@matterm:~$";

void joinRoom(Pc &pc, Host &host) {
    pc.apply(pc.sys.chatOpened());
    EXPECT(!host.sent.empty());
    std::string name = host.sent.back().substr(6); // After "hello\t".
    pc.apply(pc.sys.chatLine("welcome\t" + name));
    pc.apply(pc.sys.chatLine("users\t" + name + ",bo"));
    pc.apply(pc.sys.chatLine("ready"));
    pc.apply(pc.sys.chatLine("online\t2"));
    pc.advance(200);
}
} // namespace

TEST_SUIT_BEGIN(FakeosChat)

TEST_CASE("Without a server the chat command does not exist") {
    Host host;
    Pc pc(withChat(host, false));
    pc.run("chat");
    EXPECT(pc.shows("command not found"));
    pc.run("clear");
    pc.run("help");
    EXPECT(!pc.shows("chat"));
    pc.run("clear");
    pc.run("ls /bin");
    EXPECT(!pc.shows("chat"));
    pc.run("clear");
    pc.run("man chat");
    EXPECT(pc.shows("No manual entry"));
    EXPECT_EQ(host.opened, 0);
}

TEST_CASE("The notice comes first and n connects nothing") {
    Host host;
    Pc pc(withChat(host));
    pc.run("help");
    EXPECT(pc.shows("chat"));
    pc.run("chat");
    EXPECT(pc.shows("Live chat, draft sent as you type."));
    EXPECT(pc.shows("chat.example.org"));
    EXPECT(pc.shows("sees your IP address"));
    EXPECT(pc.shows("Connect? [y/N]"));
    EXPECT_EQ(host.opened, 0);
    pc.type("n");
    EXPECT(pc.shows("Not connecting."));
    EXPECT_EQ(host.opened, 0);
    EXPECT(pc.last() == kPrompt);
    pc.run("chat");
    pc.type("\r"); // Enter alone is no either.
    EXPECT(pc.shows("Not connecting."));
    EXPECT_EQ(host.opened, 0);
}

TEST_CASE("y opens the connection; the chat runs on the normal screen") {
    Host host;
    Pc pc(withChat(host));
    pc.run("chat");
    const auto before = pc.raw.size();
    pc.type("y");
    pc.advance(50);
    EXPECT_EQ(host.opened, 1);
    EXPECT(pc.sys.inProgram());
    const std::string chatOutput = pc.raw.substr(before);
    EXPECT(chatOutput.find("\x1b[?1049h") ==
           std::string::npos); // No alt screen.
    EXPECT(pc.shows("Mattrix chat"));
    joinRoom(pc, host);
    EXPECT(host.sent[0].rfind("hello\tguest-", 0) == 0);
    EXPECT(pc.shows("connected as guest-"));
    EXPECT(pc.shows("Mattrix chat . 2 here") || pc.shows("2 here"));
}

TEST_CASE("Typing sends the draft live, Enter locks, the bubble appears, "
          "Ctrl-C leaves") {
    Host host;
    Pc pc(withChat(host));
    pc.run("chat");
    pc.type("y");
    pc.advance(50);
    joinRoom(pc, host);
    host.sent.clear();
    pc.type("hello bo");
    pc.advance(400);
    EXPECT(!host.sent.empty());
    EXPECT(host.sent.back() == "draft\t1\thello bo");
    EXPECT(pc.shows("hello bo"));
    EXPECT(pc.shows("draft sent"));
    pc.type("\r");
    EXPECT(host.sent.back() == "lock\t1");
    pc.apply(pc.sys.chatLine("msg\t1\tbo\thi there"));
    pc.advance(200);
    EXPECT(pc.shows("hi there"));
    EXPECT(pc.shows("sent"));
    pc.type("\x03");
    pc.advance(300);
    EXPECT(!pc.sys.inProgram());
    EXPECT_EQ(host.closed, 1);
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE(
    "Others' drafts and a hostile text cannot put anything odd on the screen") {
    Host host;
    Pc pc(withChat(host));
    pc.run("chat");
    pc.type("y");
    pc.advance(50);
    joinRoom(pc, host);
    pc.apply(pc.sys.chatLine("join\tbo"));
    pc.apply(pc.sys.chatLine("draft\tbo\tthinking"));
    pc.advance(200);
    EXPECT(pc.shows("bo  typing"));
    EXPECT(pc.shows("thinking"));
    const auto before = pc.raw.size();
    pc.apply(pc.sys.chatLine(
        "msg\t2\tbo\t\x1b]52;c;AAAA\x07 bad")); // Control bytes: dropped.
    pc.apply(pc.sys.chatLine("msg\t3\tbo\t\xC2\x9B"
                             "31m\xE2\x80\xAE"
                             "evil"));
    pc.advance(200);
    const std::string added = pc.raw.substr(before);
    EXPECT(added.find("\x1b]") == std::string::npos);
    EXPECT(added.find("evil") != std::string::npos);
    EXPECT(pc.shows("31mevil"));
    EXPECT(!pc.shows("bad"));
}

TEST_CASE("When the connection closes the chat ends and the prompt returns") {
    Host host;
    Pc pc(withChat(host));
    pc.run("chat");
    pc.type("y");
    pc.advance(50);
    pc.apply(pc.sys.chatClosed()); // Could not connect at all.
    pc.advance(300);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("disconnected"));
    EXPECT(pc.last() == kPrompt);
    // And after a connection that worked.
    pc.run("chat");
    pc.type("y");
    pc.advance(50);
    joinRoom(pc, host);
    pc.apply(pc.sys.chatClosed());
    pc.advance(300);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Commands are not drafts, a resize does not break it") {
    Host host;
    Pc pc(withChat(host));
    pc.run("chat");
    pc.type("y");
    pc.advance(50);
    joinRoom(pc, host);
    host.sent.clear();
    pc.type("/who\r");
    pc.advance(300);
    EXPECT(pc.shows("here: "));
    for (const auto &l : host.sent) {
        EXPECT(l.find("/who") == std::string::npos);
    }
    pc.sys.resize(60, 20);
    pc.advance(300);
    EXPECT(pc.sys.inProgram());
    pc.type("\x03");
    pc.advance(300);
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE("Events from the host's connection reach the program through a "
          "ScriptedProgram") {
    Host host;
    fakeos::ScriptedProgram program(withChat(host));
    program.start();
    program.input("chat\r");
    program.tick(protocol::Time(std::chrono::milliseconds(10)));
    program.input("y");
    program.tick(protocol::Time(std::chrono::milliseconds(20)));
    EXPECT_EQ(host.opened, 1);
    EXPECT(program.nextDeadline().has_value());
    program.chatEvent(0, "");
    EXPECT(program.nextDeadline().has_value()); // Something is waiting.
    EXPECT(host.sent.empty());                  // Not handled before a tick.
    program.tick(protocol::Time(std::chrono::milliseconds(30)));
    EXPECT_EQ(host.sent.size(), std::size_t(1));
    EXPECT(host.sent[0].rfind("hello\tguest-", 0) == 0);
    program.chatEvent(1, "ready");
    program.chatEvent(2, "");
    const auto reaction =
        program.tick(protocol::Time(std::chrono::milliseconds(40)));
    EXPECT(reaction.output.find("disconnected") != std::string::npos ||
           reaction.output.find("offline") != std::string::npos);
}

TEST_SUIT_END
