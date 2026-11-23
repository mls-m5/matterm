#include "daemoninstall.h"
#include "mls-unit-test/unittest.h"

using namespace frontend;

namespace {
InstalledFile file(const char *name, bool running = false) {
    auto parsed = parseInstalledName(name);
    parsed->running = running;
    return *parsed;
}
} // namespace

TEST_SUIT_BEGIN(DaemonInstall)

TEST_CASE(
    "The installed name carries program, version and the first 8 of the hash") {
    mtd::Version v{1, 3, 1, "0123456789abcdef"};
    EXPECT_EQ(installedName("matterm-daemon", v),
              std::string("matterm-daemon-1.3.1-01234567"));
    EXPECT_EQ(installedName("matterm-attach", {1, 10, 0, "abcd1234"}),
              std::string("matterm-attach-1.10.0-abcd1234"));
}

TEST_CASE("Names are read back; anything else is not ours") {
    const auto a = parseInstalledName("matterm-daemon-1.3.1-01234567");
    EXPECT(a.has_value());
    EXPECT_EQ(a->program, std::string("matterm-daemon"));
    EXPECT(a->version.format == 1 && a->version.breaking == 3 &&
           a->version.additive == 1);
    EXPECT(parseInstalledName("matterm-attach-1.10.0-abcd1234").has_value());
    for (const char *bad :
         {"matterm-daemon-0123456789abcdef01234567", // The old content-hash
                                                     // name.
          "matterm-daemon",
          "matterm-daemon-1.3.1",
          "matterm-daemon-1.3.1-xyz",
          "matterm-daemon-1.3-0123abcd",
          "matterm-daemon-1.3.1-0123abcd.tmp",
          ".matterm-daemon.123",
          "matterm-server-1.3.1-01234567",
          "matterm-daemon-1.3.1-0123ABCD",
          "matterm-daemon-1.3.1-0123abcde"}) {
        EXPECT(!parseInstalledName(bad).has_value());
    }
}

TEST_CASE("Cleanup removes old versions, keeps the newest older one and never "
          "a running file") {
    const std::vector<InstalledFile> found = {
        file("matterm-daemon-1.3.1-aaaaaaa1"), // Current.
        file("matterm-daemon-1.3.0-aaaaaaa2"), // Newest older: the fallback.
        file("matterm-daemon-1.2.9-aaaaaaa3"),
        file("matterm-daemon-1.2.0-aaaaaaa4", true), // Runs.
        file("matterm-daemon-1.9.0-aaaaaaa5"), // Newer than the current: left
                                               // alone.
        file("matterm-attach-1.3.1-aaaaaaa1"),
        file("matterm-attach-1.1.0-aaaaaaa6"), // Newest (only) older attach:
                                               // kept.
    };
    const auto remove = filesToRemove(
        found,
        {"matterm-daemon-1.3.1-aaaaaaa1", "matterm-attach-1.3.1-aaaaaaa1"});
    EXPECT_EQ(remove.size(), std::size_t(1));
    EXPECT_EQ(remove[0], std::string("matterm-daemon-1.2.9-aaaaaaa3"));
}

TEST_CASE("Versions are ordered as numbers: 1.10.0 is newer than 1.9.0") {
    const std::vector<InstalledFile> found = {
        file("matterm-daemon-1.11.0-aaaaaaa1"), // Current.
        file("matterm-daemon-1.10.0-aaaaaaa2"), // Newest older.
        file("matterm-daemon-1.9.0-aaaaaaa3"),
    };
    const auto remove =
        filesToRemove(found, {"matterm-daemon-1.11.0-aaaaaaa1"});
    EXPECT_EQ(remove.size(), std::size_t(1));
    EXPECT_EQ(remove[0], std::string("matterm-daemon-1.9.0-aaaaaaa3"));
}

TEST_CASE("Two builds of the same version: the current one stays, one other is "
          "kept as fallback") {
    const std::vector<InstalledFile> found = {
        file("matterm-daemon-1.3.1-aaaaaaa1"), // Current.
        file("matterm-daemon-1.3.1-bbbbbbb2"),
        file("matterm-daemon-1.3.1-ccccccc3"),
    };
    const auto remove = filesToRemove(found, {"matterm-daemon-1.3.1-aaaaaaa1"});
    EXPECT_EQ(remove.size(), std::size_t(1));
}

TEST_CASE(
    "The listing the remote shell prints is read: name and whether it runs") {
    const auto files = parseInstalledListing(
        "matterm-daemon-1.3.0-aaaaaaa1 0\nmatterm-attach-1.3.0-aaaaaaa1 1\n"
        "junk\nmatterm-daemon-0123456789abcdef01234567 0\n\n");
    EXPECT_EQ(files.size(), std::size_t(2));
    EXPECT(!files[0].running);
    EXPECT(files[1].running);
}

TEST_SUIT_END
