#include "daemoncommands.h"
#include "mls-unit-test/unittest.h"

using namespace frontend;

TEST_SUIT_BEGIN(DaemonCommands)

TEST_CASE("The palette offers the daemon actions only when a daemon is used") {
    EXPECT(daemonCommandEntries(false).empty());
    const auto offered = daemonCommandEntries(true);
    EXPECT_EQ(offered.size(), std::size_t(3));
    EXPECT_EQ(offered[0].id, "daemon.stop");
    EXPECT_EQ(offered[0].name, "Stop daemon");
    EXPECT_EQ(offered[1].name, "Quit and keep running");
    EXPECT_EQ(offered[2].name, "Daemon status");
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

TEST_SUIT_END
