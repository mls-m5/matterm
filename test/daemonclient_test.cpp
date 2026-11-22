// Against a real matterm-daemon in a temporary directory: start it, ask it,
// attach a terminal, leave and come back, stop it.
#include "daemonclient.h"
#include "daemonlink.h"
#include "mls-unit-test/unittest.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace frontend;
using std::chrono::milliseconds;

namespace {
protocol::Time clockNow() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}

struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/matterm-daemon-test-XXXXXX";
        path = ::mkdtemp(pattern);
    }
    ~TempDir() {
        std::string command = "rm -rf '" + path + "'";
        [[maybe_unused]] const int rc = std::system(command.c_str());
    }
};

bool until(const std::function<bool()> &condition, int timeoutMs = 5000) {
    const auto end = std::chrono::steady_clock::now() + milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return condition();
}

std::string textOf(TerminalView &view) {
    std::string all;
    const auto &model = view.model();
    const auto first = model.firstRetainedRow();
    for (std::size_t row = 0; row < model.historySize() + model.rows(); ++row) {
        for (std::size_t column = 0; column < model.columns(); ++column) {
            const auto code = model.textCodePoint({first + row, column});
            all += code && *code >= 32 && *code < 127 ? char(*code) : ' ';
        }
        all += '\n';
    }
    return all;
}

TerminalView makeView() {
    TerminalView::Config config;
    config.columns = 60;
    config.rows = 10;
    return TerminalView(config);
}

std::optional<mtd::Status> statusOf(const std::string &socket) {
    std::string error;
    const auto reply = askDaemon(socket, mtd::GetStatus{}, error);
    if (reply) {
        if (const auto *status = std::get_if<mtd::Status>(&*reply)) {
            return *status;
        }
    }
    return std::nullopt;
}
} // namespace

TEST_SUIT_BEGIN(DaemonClient)

TEST_CASE("Start the daemon, run a terminal in it, leave, come back, stop it") {
    ::setenv("SHELL", "/bin/sh", 1);
    ::setenv("PS1", "$ ", 1);
    TempDir dir;
    const std::string socket = dir.path + "/d.sock";
    EXPECT(!daemonRunning(socket));
    EXPECT_EQ(ensureDaemon(socket, MATTERM_DAEMON_BIN), "");
    EXPECT(daemonRunning(socket));
    EXPECT_EQ(ensureDaemon(socket, MATTERM_DAEMON_BIN),
              ""); // Already there: nothing to do.

    auto status = statusOf(socket);
    EXPECT(status.has_value());
    EXPECT_EQ(status->sessions, std::uint32_t(0));

    std::string id;
    std::uint64_t offset = 0;
    {
        auto view = makeView();
        auto link = attachToDaemon(view, socket, "", 0, clockNow);
        EXPECT(link != nullptr);
        EXPECT(until([&] {
            link->tick();
            return !link->sessionId().empty();
        }));
        EXPECT(view.state() == TerminalView::State::Live);
        EXPECT(view.sendInput("echo hello-from-daemon\n"));
        EXPECT(until([&] {
            link->tick();
            return textOf(view).find("hello-from-daemon") != std::string::npos;
        }));
        status = statusOf(socket);
        EXPECT(status.has_value());
        EXPECT_EQ(status->sessions, std::uint32_t(1));
        EXPECT_EQ(status->attached, std::uint32_t(1));
        id = link->sessionId();
        offset = link->receivedOffset();
        view.detach();
        link->detach(); // Leave the program running.
    }
    EXPECT(until([&] {
        const auto s = statusOf(socket);
        return s && s->attached == 0;
    }));
    status = statusOf(socket);
    EXPECT_EQ(status->sessions,
              std::uint32_t(1)); // Still there, without a window.

    {
        auto view = makeView();
        auto link = attachToDaemon(
            view, socket, id, 0, clockNow); // A new window: everything again.
        EXPECT(link != nullptr);
        EXPECT(until([&] {
            link->tick();
            return textOf(view).find("hello-from-daemon") != std::string::npos;
        }));
        EXPECT(view.state() == TerminalView::State::Live);
        EXPECT(link->receivedOffset() >= offset);
        view.detach();
    }

    // Stopping: a wrong count changes nothing, the right one ends the daemon.
    std::string error;
    const auto refused = askDaemon(socket, mtd::Shutdown{5}, error);
    EXPECT(refused.has_value());
    const auto *reply =
        refused ? std::get_if<mtd::ShutdownReply>(&*refused) : nullptr;
    EXPECT(reply != nullptr);
    EXPECT(reply && !reply->done);
    EXPECT(daemonRunning(socket));
    const auto accepted = askDaemon(socket, mtd::Shutdown{1}, error);
    const auto *done =
        accepted ? std::get_if<mtd::ShutdownReply>(&*accepted) : nullptr;
    EXPECT(done && done->done);
    EXPECT(until([&] { return !daemonRunning(socket); }));
}

TEST_CASE("Marking a terminal shared shows in the daemon's list, and back") {
    ::setenv("SHELL", "/bin/sh", 1);
    TempDir dir;
    const std::string socket = dir.path + "/d.sock";
    EXPECT_EQ(ensureDaemon(socket, MATTERM_DAEMON_BIN), "");
    auto view = makeView();
    auto link = attachToDaemon(view, socket, "", 0, clockNow);
    EXPECT(link != nullptr);
    EXPECT(until([&] {
        link->tick();
        return !link->sessionId().empty();
    }));
    const auto sharedFlag = [&](bool on) {
        std::string error;
        const auto reply =
            askDaemon(socket, mtd::SetShared{link->sessionId(), on}, error);
        const auto *list =
            reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr;
        if (!list) {
            return -1;
        }
        for (const auto &entry : list->sessions) {
            if (entry.id == link->sessionId()) {
                return entry.shared ? 1 : 0;
            }
        }
        return -1;
    };
    EXPECT_EQ(sharedFlag(true), 1);
    EXPECT_EQ(sharedFlag(false), 0);
    std::string error;
    const auto unknown =
        askDaemon(socket,
                  mtd::SetShared{"00000000000000000000000000000000", true},
                  error);
    EXPECT(unknown.has_value());
    EXPECT(unknown && std::holds_alternative<mtd::ControlError>(*unknown));
    view.detach();
    std::string stop;
    askDaemon(socket, mtd::Shutdown{1}, stop);
}

TEST_CASE("No daemon: asking says so") {
    TempDir dir;
    std::string error;
    EXPECT(!askDaemon(dir.path + "/none.sock", mtd::GetStatus{}, error)
                .has_value());
    EXPECT(!error.empty());
}

TEST_SUIT_END
