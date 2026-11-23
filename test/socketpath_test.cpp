#include "mls-unit-test/unittest.h"
#include "socketpath.h"

#include <filesystem>
#include <fstream>

TEST_SUIT_BEGIN(SocketPath)

TEST_CASE("A daemon's socket name carries the version") {
    EXPECT_EQ(mtd::versionedSocketName(1, 3, 1),
              std::string("daemon-1.3.1.sock"));
    EXPECT_EQ(mtd::versionedSocketName(1, 10, 0),
              std::string("daemon-1.10.0.sock"));
}

TEST_CASE("A socket name is read back; the old name counts as 1.3.0; others "
          "are not daemons") {
    const auto v = mtd::parseSocketName("daemon-1.3.1.sock");
    EXPECT(v.has_value());
    EXPECT(v->format == 1 && v->breaking == 3 && v->additive == 1);
    const auto old = mtd::parseSocketName("daemon.sock");
    EXPECT(old.has_value());
    EXPECT(old->format == 1 && old->breaking == 3 && old->additive == 0);
    for (const char *name : {"daemon-1.3.sock",
                             "daemon-1.3.1.sock.lock",
                             "daemon-a.b.c.sock",
                             "other.sock",
                             "daemon-1.3.1.2.sock",
                             "daemon-.sock",
                             "daemon-70000.1.1.sock"}) {
        EXPECT(!mtd::parseSocketName(name).has_value());
    }
}

TEST_CASE(
    "Sockets of a folder are sorted by version as numbers, newest first") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtsXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    for (const char *name : {"daemon.sock",
                             "daemon-1.3.1.sock",
                             "daemon-1.10.0.sock",
                             "daemon-1.9.0.sock",
                             "daemon-1.3.1.sock.lock",
                             "notes.sock",
                             "daemon-2.0.0.sock"}) {
        std::ofstream(dir + "/" + name) << "x";
    }
    const auto sockets = mtd::daemonSocketsIn(dir);
    EXPECT_EQ(sockets.size(), std::size_t(5));
    EXPECT_EQ(sockets[0], dir + "/daemon-2.0.0.sock");
    EXPECT_EQ(sockets[1], dir + "/daemon-1.10.0.sock");
    EXPECT_EQ(sockets[2], dir + "/daemon-1.9.0.sock");
    EXPECT_EQ(sockets[3], dir + "/daemon-1.3.1.sock");
    EXPECT_EQ(sockets[4], dir + "/daemon.sock");
    EXPECT(mtd::daemonSocketsIn(dir + "/none").empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("A version-named socket next to a path keeps its folder") {
    EXPECT_EQ(
        mtd::versionedSiblingSocket("/run/u/matterm/daemon.sock", 1, 3, 1),
        std::string("/run/u/matterm/daemon-1.3.1.sock"));
    EXPECT_EQ(mtd::versionedSiblingSocket(
                  "/run/u/matterm/daemon-1.2.0.sock", 1, 3, 1),
              std::string("/run/u/matterm/daemon-1.3.1.sock"));
    EXPECT_EQ(mtd::versionedSiblingSocket("/tmp/x/mine.sock", 1, 3, 1),
              std::string("/tmp/x/mine-1.3.1.sock"));
    EXPECT_EQ(mtd::versionedSiblingSocket("/tmp/x/mine", 1, 3, 1),
              std::string("/tmp/x/mine-1.3.1.sock"));
}

TEST_SUIT_END
