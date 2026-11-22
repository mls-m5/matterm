#include "mls-unit-test/unittest.h"
#include "procinfo.h"
#include "sessionfiles.h"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

using namespace frontend;

namespace {
struct TempDir {
    std::string path;
    TempDir() {
        char t[] = "/tmp/matterm-sessions-XXXXXX";
        path = ::mkdtemp(t);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
SessionSnapshot make(const std::string &name, std::int64_t saved, const std::string &cmd = "vim") {
    SessionSnapshot s;
    s.name = name;
    s.saved = saved;
    SavedTab t;
    SavedPane p;
    p.cwd = "/tmp";
    p.command = {cmd, "x y"};
    p.text = "hello\nworld";
    t.root = SavedNode::leafOf(std::move(p));
    s.tabs.push_back(std::move(t));
    return s;
}
void spit(const std::string &path, const std::string &text, mode_t mode = 0600) {
    std::ofstream(path, std::ios::binary) << text;
    ::chmod(path.c_str(), mode);
}
mode_t modeOf(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : mode_t(0xffff);
}
} // namespace

TEST_SUIT_BEGIN(SessionFilesSuite)

TEST_CASE("Save creates the folder owner-only and the file 0600, load reads it back") {
    TempDir d;
    SessionFiles files(d.path + "/a/b/sessions");
    std::string error;
    EXPECT(files.save(make("work", 100), &error));
    EXPECT_EQ(modeOf(d.path + "/a/b/sessions"), mode_t(0700));
    EXPECT_EQ(modeOf(d.path + "/a/b/sessions/work.json"), mode_t(0600));
    const auto back = files.load("work");
    EXPECT(back.has_value());
    EXPECT_EQ(back->serialize(), make("work", 100).serialize());
    EXPECT_EQ(files.directory(), d.path + "/a/b/sessions");
}

TEST_CASE("Saving over a session replaces it atomically and leaves no temporary file") {
    TempDir d;
    SessionFiles files(d.path);
    EXPECT(files.save(make("work", 1, "one")));
    EXPECT(files.save(make("work", 2, "two")));
    EXPECT_EQ(files.load("work")->saved, std::int64_t(2));
    EXPECT_EQ(files.list().size(), std::size_t(1));
    EXPECT(::access((d.path + "/work.json").c_str(), F_OK) == 0);
    EXPECT(::access((d.path + "/.tmp-" + std::to_string(::getpid()) + "-work").c_str(), F_OK) != 0);
}

TEST_CASE("Invalid snapshots and names are not written") {
    TempDir d;
    SessionFiles files(d.path);
    auto bad = make("work", 1);
    bad.tabs[0].root->pane.cwd = "relative";
    std::string error;
    EXPECT(!files.save(bad, &error));
    EXPECT(!error.empty());
    EXPECT(!files.save(make("../escape", 1)));
    EXPECT(!files.load("../x").has_value());
    EXPECT(!files.remove("../x"));
    EXPECT(files.list().empty());
}

TEST_CASE("Files that are not ours to trust are skipped") {
    TempDir d;
    SessionFiles files(d.path);
    EXPECT(files.save(make("good", 5)));
    // Garbage, truncated, another name inside, oversized, group-writable, a link.
    spit(d.path + "/garbage.json", "{not json");
    const auto text = files.load("good")->serialize();
    spit(d.path + "/cut.json", text.substr(0, text.size() / 2));
    spit(d.path + "/other.json", text); // says "good"
    spit(d.path + "/huge.json", std::string(SessionSnapshot::kMaxFile + 1, ' '));
    EXPECT(files.save(make("loose", 6)));
    ::chmod((d.path + "/loose.json").c_str(), 0666);
    EXPECT(::symlink((d.path + "/good.json").c_str(), (d.path + "/link.json").c_str()) == 0);
    spit(d.path + "/not-a-name.txt", text);
    const auto list = files.list();
    EXPECT_EQ(list.size(), std::size_t(1));
    EXPECT_EQ(list[0].name, std::string("good"));
    EXPECT(!files.load("other").has_value());
    EXPECT(!files.load("link").has_value());
    EXPECT(!files.load("loose").has_value());
}

TEST_CASE("The list is newest first and counts tabs and panes") {
    TempDir d;
    SessionFiles files(d.path);
    EXPECT(files.save(make("old", 10)));
    EXPECT(files.save(make("new", 30)));
    EXPECT(files.save(make("mid", 20)));
    const auto list = files.list();
    EXPECT_EQ(list.size(), std::size_t(3));
    EXPECT_EQ(list[0].name, std::string("new"));
    EXPECT_EQ(list[2].name, std::string("old"));
    EXPECT_EQ(list[0].tabs, std::size_t(1));
    EXPECT_EQ(list[0].panes, std::size_t(1));
    EXPECT(files.remove("mid"));
    EXPECT_EQ(files.list().size(), std::size_t(2));
}

TEST_CASE("A session folder that others can write to is refused") {
    TempDir d;
    ::chmod(d.path.c_str(), 0777);
    SessionFiles files(d.path);
    std::string error;
    EXPECT(!files.save(make("work", 1), &error));
    EXPECT(files.list().empty());
}

TEST_CASE("Default folder follows XDG_CONFIG_HOME, then HOME") {
    ::setenv("XDG_CONFIG_HOME", "/xdg", 1);
    EXPECT_EQ(SessionFiles::defaultDirectory(), std::string("/xdg/matterm/sessions"));
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    ::setenv("HOME", "/home/x", 1);
    EXPECT_EQ(SessionFiles::defaultDirectory(), std::string("/home/x/.config/matterm/sessions"));
    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("HOME");
    EXPECT(SessionFiles::defaultDirectory().empty());
}

TEST_CASE("The /proc reader sees this process and nothing for a missing one") {
    LinuxProcSource proc;
    const int self = int(::getpid());
    char here[4096];
    EXPECT(::getcwd(here, sizeof here) != nullptr);
    const auto cwd = proc.cwd(self);
    EXPECT(cwd.has_value());
    EXPECT_EQ(*cwd, std::string(here));
    const auto argv = proc.cmdline(self);
    EXPECT(argv.has_value() && !argv->empty());
    EXPECT(argv->front().find("sessionfiles_test") != std::string::npos);
    EXPECT(!proc.cwd(0).has_value());
    EXPECT(!proc.cmdline(-1).has_value());
    EXPECT(!proc.cmdline(0x7ffffff0).has_value());
    const auto state = probePane(proc, self, self);
    EXPECT(state.busy); // The test binary is not a shell.
    EXPECT_EQ(state.cwd, std::string(here));
}

TEST_SUIT_END
