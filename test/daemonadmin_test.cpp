#include "daemonadmin.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"

#include <csignal>
#include <fstream>
#include <sys/wait.h>

using namespace frontend;

namespace {
DaemonInfo info(std::uint16_t breaking,
                std::uint16_t additive,
                std::uint32_t pid,
                std::uint32_t sessions,
                std::string hash = "abc1234") {
    DaemonInfo i;
    i.version = {1, breaking, additive, std::move(hash)};
    i.welcome.version = breaking;
    i.welcome.pid = pid;
    mtd::Status s;
    s.sessions = sessions;
    s.pid = pid;
    i.status = s;
    return i;
}
} // namespace

namespace {
struct Stopper {
    std::optional<DaemonInfo> daemon;
    std::optional<mtd::ShutdownReply> reply;
    std::optional<PeerCredentials> peer;
    bool answer = true;
    bool gone = true;
    std::vector<std::string> questions;
    std::vector<std::uint32_t> shutdowns, killed;
    DaemonStopTools tools() {
        DaemonStopTools t;
        t.probe = [this](const std::string &) { return daemon; };
        t.ask = [this](const std::string &q) {
            questions.push_back(q);
            return answer;
        };
        t.shutdown = [this](const std::string &, std::uint32_t count) {
            shutdowns.push_back(count);
            return reply;
        };
        t.peer = [this](const std::string &) { return peer; };
        t.terminate = [this](std::uint32_t pid) {
            killed.push_back(pid);
            return true;
        };
        t.gone = [this](const std::string &) { return gone; };
        return t;
    }
};
} // namespace

TEST_SUIT_BEGIN(DaemonAdmin)

TEST_CASE(
    "Daemon sockets are the .sock files of the folders, not the lock files") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtaXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    for (const char *name :
         {"daemon.sock", "daemon-b4.sock", "daemon.sock.lock", "notes.txt"}) {
        std::ofstream(dir + "/" + name) << "x";
    }
    const auto found = findDaemonSockets({dir, dir + "/missing"});
    EXPECT_EQ(found.size(), std::size_t(2));
    EXPECT_EQ(found[0], dir + "/daemon-b4.sock"); // Sorted.
    EXPECT_EQ(found[1], dir + "/daemon.sock");
    std::filesystem::remove_all(dir);
}

TEST_CASE("The list names socket, pid, version, hash and terminals") {
    std::vector<DaemonEntry> rows;
    rows.push_back({"/s/daemon.sock", info(3, 1, 100, 2), 100, {}});
    rows.push_back({"/s/daemon-b4.sock", info(4, 0, 200, 0, ""), 200, {}});
    rows.push_back(
        {"/s/old.sock", std::nullopt, 300, "does not answer the handshake"});
    rows.push_back({"/s/gone.sock",
                    std::nullopt,
                    0,
                    "nothing listens (a left-over file)"});
    const auto text = formatDaemonList(rows);
    EXPECT(text.find("/s/daemon.sock") != std::string::npos);
    EXPECT(text.find("pid 100") != std::string::npos);
    EXPECT(text.find("1.3.1+abc1234") != std::string::npos);
    EXPECT(text.find("2 terminals") != std::string::npos);
    EXPECT(text.find("0 terminals") != std::string::npos);
    EXPECT(text.find("pid 300") != std::string::npos);
    EXPECT(text.find("does not answer the handshake") != std::string::npos);
    EXPECT(text.find("left-over") != std::string::npos);
    EXPECT(formatDaemonList({}).find("No daemon") != std::string::npos);
}

TEST_CASE(
    "Stopping asks first, names the terminals, then shuts the daemon down") {
    Stopper s;
    s.daemon = info(3, 1, 100, 3);
    s.reply = mtd::ShutdownReply{true, 3};
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(r.stopped);
    EXPECT_EQ(s.questions.size(), std::size_t(1));
    EXPECT(s.questions[0].find("3 terminals") != std::string::npos);
    EXPECT(s.questions[0].find("/s/daemon.sock") != std::string::npos);
    EXPECT(s.questions[0].find("1.3.1+abc1234") != std::string::npos);
    EXPECT_EQ(s.shutdowns.size(), std::size_t(1));
    EXPECT_EQ(s.shutdowns[0], 3u);
    EXPECT(s.killed.empty());
}

TEST_CASE("Saying no stops nothing") {
    Stopper s;
    s.daemon = info(3, 1, 100, 1);
    s.answer = false;
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(!r.stopped);
    EXPECT(s.shutdowns.empty() && s.killed.empty());
}

TEST_CASE("A count that changed is reported and nothing ends") {
    Stopper s;
    s.daemon = info(3, 1, 100, 3);
    s.reply = mtd::ShutdownReply{false, 5};
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(!r.stopped);
    EXPECT(r.message.find("5 terminals") != std::string::npos);
    EXPECT(s.killed.empty());
}

TEST_CASE("A daemon of another breaking version is still asked to shut down") {
    Stopper s;
    s.daemon = info(7, 0, 100, 2);
    s.reply = mtd::ShutdownReply{true, 2};
    EXPECT(stopDaemon("/s/daemon.sock", s.tools()).stopped);
    EXPECT_EQ(s.shutdowns.size(), std::size_t(1));
}

TEST_CASE(
    "A daemon that does not answer the handshake is ended through its pid") {
    Stopper s;
    s.peer = PeerCredentials{4711, ::geteuid()};
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(r.stopped);
    EXPECT_EQ(s.killed.size(), std::size_t(1));
    EXPECT_EQ(s.killed[0], 4711u);
    EXPECT(s.questions[0].find("does not answer") != std::string::npos);
    EXPECT(s.questions[0].find("4711") != std::string::npos);
    EXPECT(s.questions[0].find("terminals") !=
           std::string::npos); // Says it cannot count them.
}

TEST_CASE("A shutdown that gets no answer falls back to the pid") {
    Stopper s;
    s.daemon = info(3, 1, 100, 1);
    s.reply = std::nullopt;
    s.peer = PeerCredentials{100, ::geteuid()};
    EXPECT(stopDaemon("/s/daemon.sock", s.tools()).stopped);
    EXPECT_EQ(s.killed.size(), std::size_t(1));
}

TEST_CASE("Another user's process is never signalled") {
    Stopper s;
    s.peer = PeerCredentials{4711, ::geteuid() + 1};
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(!r.stopped);
    EXPECT(s.killed.empty());
}

TEST_CASE("Nothing on the socket is said plainly") {
    Stopper s;
    const auto r = stopDaemon("/s/daemon.sock", s.tools());
    EXPECT(!r.stopped);
    EXPECT(s.questions.empty());
    EXPECT(r.message.find("/s/daemon.sock") != std::string::npos);
}

TEST_CASE("A real daemon is listed with its pid and version, and stopped") {
    Running r;
    EXPECT(r.started);
    const auto rows = listDaemons({r.path}, realProbe());
    EXPECT_EQ(rows.size(), std::size_t(1));
    EXPECT(rows[0].info.has_value());
    EXPECT_EQ(rows[0].pid, std::uint32_t(::getpid()));
    EXPECT(rows[0].info->version == mtd::ownVersion());
    DaemonStopTools tools =
        realStopTools([](const std::string &) { return true; });
    const auto result = stopDaemon(r.path, tools);
    EXPECT(result.stopped);
    r.join();
    EXPECT(!std::filesystem::exists(r.path));
}

TEST_CASE(
    "A process that only listens on a socket is found by its pid and ended") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtaXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    const std::string path = dir + "/alien.sock";
    const pid_t child = ::fork();
    if (child == 0) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        (void)::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
        (void)::listen(fd, 4);
        for (;;) {
            const int c = ::accept(fd, nullptr, nullptr);
            if (c >= 0) {
                ::write(
                    c, "\0\0\0\3xyz", 7); // A frame that is no control message.
                ::close(c);
            }
        }
    }
    for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    const auto peer = peerCredentials(path);
    EXPECT(peer.has_value());
    EXPECT_EQ(peer->pid, std::uint32_t(child));
    EXPECT_EQ(peer->uid, std::uint32_t(::geteuid()));
    const auto rows = listDaemons({path}, realProbe());
    EXPECT(!rows[0].info.has_value());
    EXPECT_EQ(rows[0].pid, std::uint32_t(child));
    DaemonStopTools tools =
        realStopTools([](const std::string &) { return true; });
    EXPECT(stopDaemon(path, tools).stopped);
    int status = 0;
    EXPECT_EQ(::waitpid(child, &status, 0), child);
    EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    std::filesystem::remove_all(dir);
}

TEST_SUIT_END
