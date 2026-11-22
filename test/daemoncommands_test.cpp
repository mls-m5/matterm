#include "daemoncommands.h"
#include "mls-unit-test/unittest.h"
#include "sessioncommands.h"

using namespace frontend;

TEST_SUIT_BEGIN(DaemonCommands)

TEST_CASE("The fallback note names the reason on plain lines") {
    const auto note = daemonFallbackNote("cannot connect\x1b[31m\n");
    std::string s;
    for (const auto c : note) s += c < 128 ? char(c) : '?';
    EXPECT(s.find("could not be started (cannot connect [31m )") != std::string::npos);
    EXPECT(s.find("\x1b") == std::string::npos);
    EXPECT(s.find("end with it") != std::string::npos);
    EXPECT(daemonFallbackNote("").size() > 20);
    EXPECT(daemonFallbackNote(std::string(1000, 'x')).size() < 400);
}

TEST_CASE("The palette offers the daemon actions only when a daemon is used") {
    EXPECT(daemonCommandEntries(false).empty());
    const auto offered = daemonCommandEntries(true);
    EXPECT_EQ(offered.size(), std::size_t(3));
    EXPECT_EQ(offered[0].id, "daemon.stop");
    EXPECT_EQ(offered[0].name, "Stop daemon");
    EXPECT_EQ(offered[1].name, "Quit and keep running");
    EXPECT_EQ(offered[2].name, "Daemon status");
}

TEST_CASE("Attach entries: detach and move only for a daemon pane, attach-all only with leftovers") {
    const auto names = [](const AttachState &s) {
        std::string all;
        for (const auto &e : attachCommandEntries(s)) all += e.name + "|";
        return all;
    };
    EXPECT_EQ(names({}), std::string("Attach terminal...|"));
    EXPECT(names({1, 2, false}).find("Attach all leftover terminals") != std::string::npos);
    const auto alone = names({1, 0, true});
    EXPECT(alone.find("Detach terminal (keep running)") != std::string::npos);
    EXPECT(alone.find("Move terminal to new tab") == std::string::npos); // nothing to move out of
    EXPECT(names({2, 0, true}).find("Move terminal to new tab") != std::string::npos);
    EXPECT(alone.find("Rename terminal...") != std::string::npos);
    EXPECT(names({}).find("Rename terminal") == std::string::npos);
}

TEST_CASE("The attach list shows folder, command and who has the terminal; own terminals are left out") {
    std::vector<AttachableSession> all(4);
    all[0] = {"a", "/home/me/project/src", {}, false, false, false};
    all[1] = {"b", "/srv", {"vim", "notes.txt"}, true, true, false};
    all[2] = {"c", "/home/me", {}, true, false, true}; // this window
    all[3] = {"d", "", {"x\x1b[31m"}, false, false, false};
    const auto items = attachItems(all);
    EXPECT_EQ(items.size(), std::size_t(3));
    EXPECT_EQ(items[0].label, std::string(".../project/src"));
    EXPECT_EQ(items[0].detail, std::string("shell"));
    EXPECT_EQ(items[0].action, std::string("daemon.attach.go:a"));
    EXPECT_EQ(items[1].label, std::string("/srv").substr(1));
    EXPECT_EQ(items[1].detail, std::string("vim notes.txt (in another window) (shared)"));
    EXPECT_EQ(items[2].label, std::string("terminal"));
    EXPECT(items[2].detail.find('\x1b') == std::string::npos);
    all[0].name = "build box";
    EXPECT_EQ(attachItems(all)[0].label, std::string("build box (.../project/src)"));
    EXPECT_EQ(takenOverNotice(true, "build box"),
              std::string("a shared terminal 'build box' was taken over from another window"));
    EXPECT_EQ(takenOverNotice(false, ""), std::string("a terminal was taken over by another window"));
    EXPECT_EQ(shortFolder("/"), std::string("/"));
    EXPECT_EQ(shortFolder("/home/me/"), std::string("home/me"));
}

TEST_CASE("Taking a terminal over asks first and defaults to Cancel; attach-all names the count") {
    const auto dialog = takeOverDialog("abc", "/srv");
    EXPECT_EQ(dialog->selected(), std::size_t(1));
    EXPECT_EQ(attachAllDialog(2)->selected(), std::size_t(0));
}

TEST_CASE("The stop question names how many programs end") {
    EXPECT(stopDaemonQuestion(0).find("No programs") != std::string::npos);
    EXPECT(stopDaemonQuestion(1).find("1 program ") != std::string::npos);
    EXPECT(stopDaemonQuestion(7).find("7 programs") != std::string::npos);
}

TEST_CASE("Stopping needs a confirmation that defaults to Cancel") {
    const auto dialog = stopDaemonDialog(3);
    EXPECT_EQ(dialog->selected(), std::size_t(1));
}

TEST_CASE("Status text") {
    DaemonStatus status;
    status.socketPath = "/run/user/1/matterm.sock";
    EXPECT(formatDaemonStatus(status).find("not running") != std::string::npos);
    status.connected = true;
    EXPECT_EQ(formatDaemonStatus(status), "daemon: 0 programs");
    status.programs = 3;
    status.attached = 3;
    EXPECT_EQ(formatDaemonStatus(status), "daemon: 3 programs, 3 shown");
    status.attached = 1;
    EXPECT_EQ(formatDaemonStatus(status),
              "daemon: 3 programs, 1 shown, 2 running without a window");
    status.programs = 1;
    status.attached = 0;
    EXPECT_EQ(formatDaemonStatus(status),
              "daemon: 1 program, 0 shown, 1 running without a window");
}

TEST_CASE("Sharing entries follow what is shared now") {
    const auto ids = [](const ShareState &state) {
        std::string all;
        for (const auto &e : shareCommandEntries(state)) {
            all += e.id + " ";
        }
        return all;
    };
    EXPECT_EQ(ids({2, 0, false}), "share.pane share.tab ");
    EXPECT_EQ(ids({2, 1, true}), "share.pane.stop share.tab share.tab.stop ");
    EXPECT_EQ(ids({2, 2, true}), "share.pane.stop share.tab.stop ");
    EXPECT_EQ(ids({2, 1, false}), "share.pane share.tab share.tab.stop ");
    EXPECT_EQ(shareCommandEntries({1, 0, false})[0].name,
              "Share this terminal");
}

TEST_CASE("Deleting a saved session asks first, defaults to Cancel and warns when it is in use") {
    EXPECT_EQ(sessionCommandEntries().size(), std::size_t(1));
    EXPECT_EQ(sessionCommandEntries()[0].name, std::string("Delete session..."));
    EXPECT_EQ(deleteSessionDialog("work", false)->selected(), std::size_t(1));
    EXPECT_EQ(deleteSessionDialog("work\x1b", true)->selected(), std::size_t(1));
}

TEST_SUIT_END
