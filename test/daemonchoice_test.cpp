#include "daemonchoice.h"
#include "mls-unit-test/unittest.h"

#include <map>

using namespace frontend;

namespace {
mtd::Version version(std::uint16_t breaking,
                     std::uint16_t additive,
                     std::string hash = "h") {
    return {1, breaking, additive, std::move(hash)};
}
mtd::Version own() {
    return mtd::ownVersion();
}
std::string ownName() {
    const auto v = own();
    return "daemon-" + std::to_string(v.format) + "." +
           std::to_string(v.breaking) + "." + std::to_string(v.additive) +
           ".sock";
}

// A world of fake daemons by socket path.
struct World {
    std::map<std::string, mtd::Version> daemons;
    std::map<std::string, unsigned> terminals;
    std::vector<std::string> started, questions;
    bool answer = true;
    std::string startError;
    DaemonChoiceTools tools() {
        DaemonChoiceTools t;
        t.newSocket = "/s/" + ownName();
        t.list = [this] {
            std::vector<std::string> sockets;
            for (const auto &d : daemons) {
                if (d.first.rfind("/s/", 0) == 0) {
                    sockets.push_back(d.first);
                }
            }
            return sockets;
        };
        t.probe =
            [this](const std::string &socket) -> std::optional<DaemonInfo> {
            const auto it = daemons.find(socket);
            if (it == daemons.end()) {
                return std::nullopt;
            }
            DaemonInfo info;
            info.version = it->second;
            info.welcome.version = it->second.breaking;
            info.welcome.pid = 4242;
            mtd::Status status;
            status.sessions = terminals[socket];
            info.status = status;
            return info;
        };
        t.start = [this](const std::string &socket) -> std::string {
            if (!startError.empty()) {
                return startError;
            }
            started.push_back(socket);
            daemons[socket] = own();
            return {};
        };
        t.ask = [this](const std::string &question) {
            questions.push_back(question);
            return answer;
        };
        return t;
    }
};
} // namespace

TEST_SUIT_BEGIN(DaemonChoice)

TEST_CASE(
    "With no daemon at all a new one is started on the versioned socket") {
    World w;
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(choice.error.empty());
    EXPECT_EQ(choice.socket, "/s/" + ownName());
    EXPECT_EQ(w.started.size(), std::size_t(1));
    EXPECT(w.questions.empty());
}

TEST_CASE(
    "Without permission to start, nothing is started and the socket is named") {
    World w;
    DaemonChoiceRequest request;
    request.startMissing = false;
    const auto choice = chooseDaemon(request, w.tools());
    EXPECT(choice.error.empty());
    EXPECT_EQ(choice.socket, "/s/" + ownName());
    EXPECT(w.started.empty());
}

TEST_CASE("A daemon of the same version is simply used, wherever its socket is "
          "named") {
    World w;
    w.daemons["/s/daemon.sock"] = own();
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(choice.error.empty());
    EXPECT_EQ(choice.socket, std::string("/s/daemon.sock"));
    EXPECT(choice.note.empty());
    EXPECT(w.started.empty() && w.questions.empty());
}

TEST_CASE("A different additive number is a note, either way") {
    for (const int delta : {-1, +1}) {
        World w;
        auto v = own();
        v.additive = std::uint16_t(int(v.additive) + delta);
        v.hash = "other";
        w.daemons["/s/daemon-x.sock"] = v;
        const auto choice = chooseDaemon({}, w.tools());
        EXPECT(choice.error.empty());
        EXPECT_EQ(choice.socket, std::string("/s/daemon-x.sock"));
        EXPECT(choice.note.find(mtd::toString(v)) != std::string::npos);
        EXPECT(w.questions.empty() && w.started.empty());
    }
}

TEST_CASE("Of several compatible daemons the newest is offered, and declining "
          "is an error") {
    World w;
    auto older = own();
    older.additive = 0;
    auto newer = own();
    newer.additive = 4;
    w.daemons["/s/a.sock"] = older;
    w.daemons["/s/b.sock"] = newer;
    w.terminals["/s/a.sock"] = 2;
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(choice.error.empty());
    EXPECT_EQ(choice.socket, std::string("/s/b.sock"));
    EXPECT_EQ(w.questions.size(), std::size_t(1));
    EXPECT(w.questions[0].find("/s/a.sock") != std::string::npos);
    EXPECT(w.questions[0].find("2 terminals") != std::string::npos);
    EXPECT(w.questions[0].find("Use the newest, /s/b.sock") !=
           std::string::npos);
    w.answer = false;
    const auto declined = chooseDaemon({}, w.tools());
    EXPECT(declined.socket.empty());
    EXPECT(declined.error.find("--daemon-socket") != std::string::npos);
}

TEST_CASE("Versions are compared as numbers: 1.10 is newer than 1.9") {
    World w;
    mtd::Version a{1, own().breaking, 9, ""}, b{1, own().breaking, 10, ""};
    w.daemons["/s/a.sock"] = a;
    w.daemons["/s/b.sock"] = b;
    EXPECT_EQ(chooseDaemon({}, w.tools()).socket, std::string("/s/b.sock"));
}

TEST_CASE("Only incompatible daemons: refused, and a new daemon offered next "
          "to them") {
    World w;
    w.daemons["/s/daemon.sock"] = version(2, 0, "old");
    w.terminals["/s/daemon.sock"] = 3;
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(choice.error.empty());
    EXPECT_EQ(choice.socket, "/s/" + ownName());
    EXPECT_EQ(w.started.size(), std::size_t(1));
    EXPECT_EQ(w.questions.size(), std::size_t(1));
    const auto &q = w.questions[0];
    EXPECT(q.find("1.2.0+old") != std::string::npos);
    EXPECT(q.find(mtd::toString(own())) != std::string::npos);
    EXPECT(q.find("3 terminals") != std::string::npos);
    EXPECT(q.find("/s/" + ownName()) != std::string::npos);
    EXPECT(w.daemons.count("/s/daemon.sock")); // The old one is left alone.
}

TEST_CASE("Declining the offer is an error that says what to do") {
    World w;
    w.daemons["/s/daemon.sock"] = version(2, 0);
    w.answer = false;
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(!choice.error.empty());
    EXPECT(choice.socket.empty());
    EXPECT(w.started.empty());
    EXPECT(choice.error.find("--daemon-stop") != std::string::npos);
}

TEST_CASE(
    "An incompatible daemon is skipped when a compatible one runs beside it") {
    World w;
    w.daemons["/s/daemon.sock"] = version(2, 0);
    w.daemons["/s/" + ownName().substr(0)] = own();
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT_EQ(choice.socket, "/s/" + ownName());
    EXPECT(w.questions.empty() && w.started.empty());
}

TEST_CASE(
    "Where no offer can be made (another computer) it is a plain refusal") {
    World w;
    w.daemons["/s/daemon.sock"] = version(4, 0);
    DaemonChoiceRequest request;
    request.offerNew = false;
    const auto choice = chooseDaemon(request, w.tools());
    EXPECT(!choice.error.empty());
    EXPECT(w.questions.empty() && w.started.empty());
    EXPECT(choice.error.find("1.4.0") != std::string::npos);
}

TEST_CASE("A daemon that cannot be started is reported") {
    World w;
    w.startError = "no space";
    const auto choice = chooseDaemon({}, w.tools());
    EXPECT(choice.error.find("no space") != std::string::npos);
}

TEST_CASE("--daemon-socket wins: that daemon, started if missing") {
    World w;
    w.daemons["/s/" + ownName()] = own(); // Would be chosen otherwise.
    DaemonChoiceRequest request;
    request.socket = "/x/mine.sock";
    const auto started = chooseDaemon(request, w.tools());
    EXPECT_EQ(started.socket, std::string("/x/mine.sock"));
    EXPECT_EQ(w.started.size(), std::size_t(1));
    const auto again = chooseDaemon(request, w.tools());
    EXPECT_EQ(again.socket, std::string("/x/mine.sock"));
    EXPECT_EQ(w.started.size(), std::size_t(1));
}

TEST_CASE("--daemon-socket on an incompatible daemon offers one next to it") {
    World w;
    w.daemons["/x/mine.sock"] = version(2, 0);
    DaemonChoiceRequest request;
    request.socket = "/x/mine.sock";
    const auto choice = chooseDaemon(request, w.tools());
    EXPECT(choice.error.empty());
    const auto v = own();
    EXPECT_EQ(choice.socket,
              "/x/mine-" + std::to_string(v.format) + "." +
                  std::to_string(v.breaking) + "." +
                  std::to_string(v.additive) + ".sock");
    EXPECT_EQ(w.questions.size(), std::size_t(1));
}

TEST_SUIT_END
