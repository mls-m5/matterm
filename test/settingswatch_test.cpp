#include "mls-unit-test/unittest.h"
#include "settingsfile.h"
#include "settingswatch.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using namespace frontend::settings;

namespace {
struct FakeFs : FileSystem {
    struct Entry {
        std::string text;
        std::int64_t mtime = 0;
    };
    std::map<std::string, Entry> files;
    int stats = 0, reads = 0, writes = 0;
    std::int64_t clock = 1;
    bool failWrite = false;
    std::optional<FileStat> stat(const std::string &path) override {
        ++stats;
        const auto it = files.find(path);
        if (it == files.end()) {
            return std::nullopt;
        }
        return FileStat{it->second.mtime, it->second.text.size()};
    }
    bool read(const std::string &path, std::string &out) override {
        ++reads;
        const auto it = files.find(path);
        if (it == files.end()) {
            return false;
        }
        out = it->second.text;
        return true;
    }
    bool writeAtomic(const std::string &path,
                     const std::string &text,
                     std::string &error) override {
        if (failWrite) {
            error = "disk full";
            return false;
        }
        ++writes;
        files[path] = {text, ++clock};
        return true;
    }
    void put(const std::string &path,
             const std::string &text) { // Somebody else writes the file.
        files[path] = {text, ++clock};
    }
};
const std::string kPath = "/home/u/.config/matterm/settings.json";
} // namespace

TEST_SUIT_BEGIN(SettingsWatch)

TEST_CASE("The file is in $XDG_CONFIG_HOME/matterm, else ~/.config/matterm") {
    EXPECT_EQ(settingsPath("/x/cfg", "/home/u"),
              std::string("/x/cfg/matterm/settings.json"));
    EXPECT_EQ(settingsPath(nullptr, "/home/u"),
              std::string("/home/u/.config/matterm/settings.json"));
    EXPECT_EQ(settingsPath("", "/home/u"),
              std::string("/home/u/.config/matterm/settings.json"));
    EXPECT_EQ(
        settingsPath("relative", "/home/u"),
        std::string(
            "/home/u/.config/matterm/settings.json")); // Not absolute: ignored.
    EXPECT_EQ(settingsPath(nullptr, nullptr), std::string());
}

TEST_CASE("No file: the defaults, and nothing to report") {
    FakeFs fs;
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    EXPECT(watcher.settings().sameValues(Settings()));
    EXPECT(watcher.error().empty());
}

TEST_CASE(
    "The file is read at once, and watched about once a second with one stat") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("blue"));
    fs.stats = fs.reads = 0;
    now = 10;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
    now = 900;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
    EXPECT_EQ(fs.stats, 0); // Not yet a second.
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
    EXPECT_EQ(fs.stats, 1);
    EXPECT_EQ(fs.reads, 0); // Unchanged: not read.
    now = 1500;
    watcher.poll();
    EXPECT_EQ(fs.stats, 1);
    now = 2100;
    watcher.poll();
    EXPECT_EQ(fs.stats, 2);
    EXPECT_EQ(fs.reads, 0);
}

TEST_CASE("A changed file is reloaded; the new values show") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.put(kPath, R"({"theme":"red","tabActivity":"off"})");
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::Changed);
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("red"));
    EXPECT_EQ(watcher.settings().choice("tabActivity"), std::string("off"));
    now = 2000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
}

TEST_CASE("A new file that says the same is read but is no change") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.put(kPath, R"({"theme":"blue","comment":"touched"})");
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
    EXPECT(fs.reads >= 2);
}

TEST_CASE("A size change with the same time is seen too") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.files[kPath].text = R"({"theme":"green" })"; // Same mtime, other size.
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::Changed);
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("green"));
}

TEST_CASE("A broken file is reported once and the old values stay; fixing it "
          "recovers") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.put(kPath, R"({"theme":)");
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::Failed);
    EXPECT(!watcher.error().empty());
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("blue"));
    now = 2000;
    EXPECT(watcher.poll() ==
           SettingsWatcher::Event::None); // The same broken file: nothing new
                                          // to say.
    EXPECT(!watcher.error().empty()); // But it is still the standing problem.
    fs.put(kPath, R"({"theme":"green"})");
    now = 3000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::Changed);
    EXPECT(watcher.error().empty());
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("green"));
}

TEST_CASE("Values that do not fit are warnings, not an error") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"purple"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    EXPECT(watcher.error().empty());
    EXPECT_EQ(watcher.warnings().size(), std::size_t(1));
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("grey"));
}

TEST_CASE("A deleted file goes back to the defaults") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.files.erase(kPath);
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::Changed);
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("grey"));
    now = 2000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
}

TEST_CASE(
    "reloadNow reads at once, also an unchanged file, and says what it found") {
    FakeFs fs;
    fs.put(kPath, R"({"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    fs.reads = 0;
    EXPECT(watcher.reloadNow() == SettingsWatcher::Event::None);
    EXPECT_EQ(fs.reads, 1);
    fs.put(kPath, R"({"theme":"red"})");
    EXPECT(watcher.reloadNow() ==
           SettingsWatcher::Event::Changed); // No waiting for the second.
    fs.put(kPath, "nonsense");
    EXPECT(watcher.reloadNow() == SettingsWatcher::Event::Failed);
}

TEST_CASE("Saving writes the file with everything in it, and the watcher does "
          "not see its own write as news") {
    FakeFs fs;
    fs.put(kPath, R"({"future":[1],"theme":"blue"})");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    auto edited = watcher.settings();
    std::string error;
    EXPECT(edited.set("theme", "green", error));
    EXPECT(watcher.save(edited, error));
    EXPECT_EQ(fs.writes, 1);
    EXPECT(fs.files[kPath].text.find("future") != std::string::npos);
    EXPECT_EQ(watcher.settings().choice("theme"), std::string("green"));
    now = 1000;
    EXPECT(watcher.poll() == SettingsWatcher::Event::None);
    fs.failWrite = true;
    EXPECT(!watcher.save(edited, error));
    EXPECT_EQ(error, std::string("disk full"));
}

TEST_CASE("Saving over a broken file is refused: the user's text is not thrown "
          "away") {
    FakeFs fs;
    fs.put(kPath, "{ this is being edited");
    std::int64_t now = 0;
    SettingsWatcher watcher(fs, kPath, [&] { return now; });
    EXPECT(!watcher.error().empty());
    std::string error;
    EXPECT(!watcher.save(Settings(), error));
    EXPECT(error.find("broken") != std::string::npos);
    EXPECT_EQ(fs.writes, 0);
}

TEST_CASE(
    "The real file system: atomic write, private permissions, reading, stat") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtsetXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    const std::string path = dir + "/cfg/matterm/settings.json";
    RealFileSystem fs;
    EXPECT(!fs.stat(path).has_value());
    std::string error;
    EXPECT(fs.writeAtomic(path, "{\"theme\":\"red\"}\n", error));
    EXPECT_EQ(error, std::string());
    struct stat st {};
    EXPECT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0600);
    EXPECT_EQ(::stat((dir + "/cfg/matterm").c_str(), &st), 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0700);
    std::string text;
    EXPECT(fs.read(path, text));
    EXPECT_EQ(text, std::string("{\"theme\":\"red\"}\n"));
    const auto info = fs.stat(path);
    EXPECT(info.has_value() && info->size == text.size());
    EXPECT(fs.writeAtomic(path, "{}", error)); // Replaces.
    EXPECT(fs.read(path, text));
    EXPECT_EQ(text, std::string("{}"));
    int leftovers = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(dir + "/cfg/matterm")) {
        leftovers += entry.path().filename() != "settings.json";
    }
    EXPECT_EQ(leftovers, 0); // No temporary file left.
    // A directory that cannot be written is an error, and nothing is half
    // written.
    EXPECT(!fs.writeAtomic("/proc/nowhere/settings.json", "x", error));
    EXPECT(!error.empty());
    std::filesystem::remove_all(dir);
}

TEST_SUIT_END
