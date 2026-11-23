#include "daemonclient.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"
#include "version.h"

using namespace frontend;

namespace {
// Runs `matterm-daemon --ensure --print-socket` with a private socket folder;
// returns what it printed.
std::string ensureOutput(const std::string &runtime) {
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    // NOLINTNEXTLINE(cert-env33-c): runs our own daemon binary
    FILE *pipe = ::popen(
        (std::string(MATTERM_DAEMON_BIN) + " --ensure --print-socket 2>&1")
            .c_str(),
        "r");
    std::string text;
    char buffer[256];
    while (pipe && std::fgets(buffer, sizeof buffer, pipe)) {
        text += buffer;
    }
    if (pipe) {
        ::pclose(pipe);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}
} // namespace

TEST_SUIT_BEGIN(DaemonVersion)

TEST_CASE("A version prints and reads back through the Welcome text") {
    mtd::Version v;
    v.format = 1;
    v.breaking = 3;
    v.additive = 7;
    v.hash = "abc1234";
    EXPECT_EQ(mtd::toString(v), std::string("1.3.7+abc1234"));
    const auto back =
        mtd::versionFromWelcome("matterm-daemon " + mtd::toString(v), 3);
    EXPECT(back == v);
    EXPECT_EQ(back.hash, std::string("abc1234"));
    v.hash.clear();
    EXPECT_EQ(mtd::toString(v), std::string("1.3.7"));
    EXPECT(mtd::versionFromWelcome(mtd::toString(v), 3) == v);
}

TEST_CASE(
    "A daemon from before the scheme is breaking = its number, additive 0") {
    for (const char *text : {"matterm-daemon",
                             "",
                             "matterm-daemon-e2e",
                             "x 1.2",
                             "x 1.2.3.4",
                             "x a.b.c",
                             "1..2"}) {
        const auto v = mtd::versionFromWelcome(text, 3);
        EXPECT_EQ(int(v.format), 1);
        EXPECT_EQ(int(v.breaking), 3);
        EXPECT_EQ(int(v.additive), 0);
        EXPECT(v.hash.empty());
    }
    EXPECT_EQ(int(mtd::versionFromWelcome("matterm-daemon", 2).breaking), 2);
}

TEST_CASE("When the text and the handshake disagree the handshake wins") {
    const auto v = mtd::versionFromWelcome("matterm-daemon 1.9.4+x", 3);
    EXPECT_EQ(int(v.breaking), 3);
    EXPECT_EQ(int(v.additive), 0);
}

TEST_CASE("Only format and breaking decide; additive is a note; the hash is "
          "ignored") {
    mtd::Version mine{1, 3, 2, "aaa"};
    EXPECT(mtd::compatibility(mine, {1, 3, 2, "bbb"}) ==
           mtd::Compatibility::Same);
    EXPECT(mtd::compatibility(mine, {1, 3, 1, ""}) ==
           mtd::Compatibility::DaemonOlder);
    EXPECT(mtd::compatibility(mine, {1, 3, 9, ""}) ==
           mtd::Compatibility::DaemonNewer);
    EXPECT(mtd::compatibility(mine, {1, 4, 2, ""}) ==
           mtd::Compatibility::Incompatible);
    EXPECT(mtd::compatibility(mine, {1, 2, 2, ""}) ==
           mtd::Compatibility::Incompatible);
    EXPECT(mtd::compatibility(mine, {2, 3, 2, ""}) ==
           mtd::Compatibility::Incompatible);
}

TEST_CASE("A daemon tells its version in the Welcome") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(c.sendControl(mtd::ControlHello{}));
    const auto reply = c.controlReply();
    EXPECT(reply.has_value());
    const auto &welcome = std::get<mtd::ControlWelcome>(*reply);
    EXPECT_EQ(int(welcome.version), int(mtd::kControlBreaking));
    const auto v = mtd::versionFromWelcome(welcome.build, welcome.version);
    EXPECT_EQ(int(v.format), int(mtd::kControlFormat));
    EXPECT_EQ(int(v.breaking), int(mtd::kControlBreaking));
    EXPECT_EQ(int(v.additive), int(mtd::kControlAdditive));
    EXPECT(!v.hash.empty());
}

TEST_CASE("probeDaemon reports the daemon's version, pid and the verdict") {
    Running r;
    EXPECT(r.started);
    std::string error;
    const auto info = probeDaemon(r.path, error);
    EXPECT(info.has_value());
    EXPECT_EQ(int(info->welcome.pid), int(::getpid()));
    EXPECT(mtd::compatibility(mtd::ownVersion(), info->version) ==
           mtd::Compatibility::Same);
    EXPECT(!probeDaemon(r.dir + "/none.sock", error).has_value());
    EXPECT(!error.empty());
}

TEST_CASE("A daemon with another breaking number is seen as incompatible and "
          "still stops") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.version.breaking = 4;
    });
    EXPECT(r.started);
    std::string error;
    const auto info = probeDaemon(r.path, error);
    EXPECT(info.has_value());
    EXPECT_EQ(int(info->version.breaking), 4);
    EXPECT(mtd::compatibility(mtd::ownVersion(), info->version) ==
           mtd::Compatibility::Incompatible);
    const auto status = askDaemon(r.path, mtd::GetStatus{}, error);
    EXPECT(status.has_value() && std::holds_alternative<mtd::Status>(*status));
    const auto stopped = askDaemon(r.path, mtd::Shutdown{0}, error);
    EXPECT(stopped.has_value() && std::get<mtd::ShutdownReply>(*stopped).done);
}

TEST_CASE("A daemon older than its client refuses everything but status and "
          "shutdown") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.version.breaking = 2;
    });
    EXPECT(r.started);
    auto ctl = controlClient(r.path); // Our hello says 3.
    EXPECT(ctl != nullptr);
    EXPECT(ctl->sendControl(mtd::ListSessions{}));
    const auto refused = ctl->controlReply();
    EXPECT(refused.has_value() &&
           std::holds_alternative<mtd::ControlError>(*refused));
    const auto &reason = std::get<mtd::ControlError>(*refused).reason;
    EXPECT(reason.find("incompatible") != std::string::npos);
    EXPECT(reason.find("1.2.") != std::string::npos); // Says what it is.
    std::string error;
    const auto status = askDaemon(r.path, mtd::GetStatus{}, error);
    EXPECT(status.has_value() && std::holds_alternative<mtd::Status>(*status));
}

TEST_CASE("A daemon newer than its client still serves it") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.version.breaking = 4;
    });
    EXPECT(r.started);
    std::string error;
    const auto list = askDaemon(r.path, mtd::ListSessions{}, error);
    EXPECT(list.has_value() && std::holds_alternative<mtd::SessionList>(*list));
}

TEST_CASE("A daemon with another additive number works as usual") {
    for (const unsigned additive : {0u, 9u}) {
        Running r(60, std::nullopt, {}, [additive](mtd::DaemonConfig &c) {
            c.version.additive = std::uint16_t(additive);
        });
        EXPECT(r.started);
        std::string error;
        const auto info = probeDaemon(r.path, error);
        EXPECT(info.has_value());
        EXPECT_EQ(int(info->version.additive), int(additive));
        EXPECT(mtd::compatibility(mtd::ownVersion(), info->version) !=
               mtd::Compatibility::Incompatible);
        const auto list = askDaemon(r.path, mtd::ListSessions{}, error);
        EXPECT(list.has_value() &&
               std::holds_alternative<mtd::SessionList>(*list));
    }
}

TEST_CASE("--ensure reuses a compatible daemon on the old socket name") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtvXXXXXX").string();
    const std::string runtime = ::mkdtemp(t.data());
    std::filesystem::create_directories(runtime + "/matterm");
    ::chmod((runtime + "/matterm").c_str(), 0700);
    {
        Running r(60, std::nullopt, runtime + "/matterm");
        r.ownsDir = false;
        // The fixture names its socket d.sock: use the real daemon file name
        // instead.
        std::filesystem::rename(r.path, runtime + "/matterm/daemon.sock");
        EXPECT(r.started);
        const auto printed = ensureOutput(runtime);
        EXPECT_EQ(printed, runtime + "/matterm/daemon.sock");
        EXPECT_EQ(mtd::daemonSocketsIn(runtime + "/matterm").size(),
                  std::size_t(1));
        r.stop();
    }
    std::filesystem::remove_all(runtime);
}

TEST_CASE("--ensure starts a new daemon on the versioned socket next to an "
          "incompatible one") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtvXXXXXX").string();
    const std::string runtime = ::mkdtemp(t.data());
    std::filesystem::create_directories(runtime + "/matterm");
    ::chmod((runtime + "/matterm").c_str(), 0700);
    std::string started;
    {
        Running r(60,
                  std::nullopt,
                  runtime + "/matterm",
                  [](mtd::DaemonConfig &c) { c.version.breaking = 2; });
        r.ownsDir = false;
        std::filesystem::rename(r.path, runtime + "/matterm/daemon.sock");
        EXPECT(r.started);
        started = ensureOutput(runtime);
        const auto own = mtd::ownVersion();
        EXPECT_EQ(started,
                  runtime + "/matterm/" +
                      mtd::versionedSocketName(
                          own.format, own.breaking, own.additive));
        EXPECT_EQ(mtd::daemonSocketsIn(runtime + "/matterm").size(),
                  std::size_t(2));
        std::string error;
        const auto stopNew = askDaemon(started, mtd::Shutdown{0}, error);
        EXPECT(stopNew.has_value() &&
               std::get<mtd::ShutdownReply>(*stopNew).done);
        r.stop();
    }
    std::filesystem::remove_all(runtime);
}

TEST_SUIT_END
