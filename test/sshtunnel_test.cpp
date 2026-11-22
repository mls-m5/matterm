// The daemon of "another computer" (here: another socket) reached through SshTunnel, with a fake ssh
// (test/fakessh.py) that relays Unix sockets like `ssh -L local:remote` does. The real matterm-daemon runs
// behind it, started by `matterm-daemon --ensure --print-socket` exactly as the tunnel does over ssh.
#include "daemonclient.h"
#include "daemonlink.h"
#include "mls-unit-test/unittest.h"
#include "sshtunnel.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

using namespace frontend;
using std::chrono::milliseconds;

namespace {
protocol::Time clockNow() {
    return std::chrono::duration_cast<protocol::Time>(std::chrono::steady_clock::now().time_since_epoch());
}

struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/matterm-ssh-test-XXXXXX";
        path = ::mkdtemp(pattern);
    }
    ~TempDir() {
        const std::string command = "rm -rf '" + path + "'";
        [[maybe_unused]] const int rc = std::system(command.c_str());
    }
};

bool until(const std::function<bool()> &condition, int timeoutMs = 20000) {
    const auto end = std::chrono::steady_clock::now() + milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
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

std::string fileText(const std::string &path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// The environment the fake ssh reads; one per test, undone by the destructor.
struct Fake {
    TempDir dir;
    std::string log = dir.path + "/ssh.log";
    Fake() {
        ::setenv("FAKE_SSH_LOG", log.c_str(), 1);
        ::setenv("FAKE_SSH_DAEMON", MATTERM_DAEMON_BIN, 1);
        ::unsetenv("FAKE_SSH_FAIL");
        ::unsetenv("FAKE_SSH_NOFORWARD");
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("PS1", "$ ", 1);
    }
    ~Fake() {
        ::unsetenv("FAKE_SSH_LOG");
        ::unsetenv("FAKE_SSH_FAIL");
        ::unsetenv("FAKE_SSH_NOFORWARD");
    }
    SshTunnelOptions options() const {
        SshTunnelOptions o;
        o.host = "user@far-away";
        o.ssh = FAKE_SSH;
        o.remoteSocket = dir.path + "/remote.sock";
        o.timeoutMs = 20000;
        return o;
    }
};

std::optional<mtd::Status> statusOf(const std::string &socket) {
    std::string error;
    const auto reply = askDaemon(socket, mtd::GetStatus{}, error);
    if (reply) {
        if (const auto *status = std::get_if<mtd::Status>(&*reply)) return *status;
    }
    return std::nullopt;
}

void stopRemote(const std::string &socket) {
    std::string error;
    askDaemon(socket, mtd::Shutdown{0}, error);
}
} // namespace

TEST_SUIT_BEGIN(SshTunnel)

TEST_CASE("The daemon over a tunnel: started there, reached here, a terminal runs in it") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    EXPECT_EQ(error, "");
    EXPECT_EQ(tunnel->remoteSocketPath(), fake.dir.path + "/remote.sock");
    EXPECT(tunnel->socketPath() != tunnel->remoteSocketPath());
    EXPECT(tunnel->alive());
    // The local end is private: its directory belongs to us alone.
    struct stat st {};
    const auto directory = tunnel->socketPath().substr(0, tunnel->socketPath().rfind('/'));
    EXPECT(::stat(directory.c_str(), &st) == 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0700);

    const auto status = statusOf(tunnel->socketPath());
    EXPECT(status.has_value());
    EXPECT_EQ(status->sessions, std::uint32_t(0));

    auto view = TerminalView(TerminalView::Config{60, 10});
    auto link = attachToDaemon(view, tunnel->socketPath(), "", 0, clockNow);
    EXPECT(link != nullptr);
    EXPECT(until([&] {
        link->tick();
        return !link->sessionId().empty();
    }));
    EXPECT(view.sendInput("echo through-the-tunnel\n"));
    EXPECT(until([&] {
        link->tick();
        return textOf(view).find("through-the-tunnel") != std::string::npos;
    }));
    // It is the daemon behind the remote socket that has the session.
    const auto remote = statusOf(fake.dir.path + "/remote.sock");
    EXPECT(remote.has_value());
    EXPECT_EQ(remote->sessions, std::uint32_t(1));
    view.detach();
    link->detach();

    // The ssh calls: the daemon asked for with --ensure, host after "--", no questions, our forward.
    const auto calls = fileText(fake.log);
    EXPECT(calls.find("\"--ensure\"") != std::string::npos);
    EXPECT(calls.find("\"--print-socket\"") != std::string::npos);
    EXPECT(calls.find("\"BatchMode=yes\"") != std::string::npos);
    EXPECT(calls.find("\"--\", \"user@far-away\"") != std::string::npos);
    EXPECT(calls.find("ExitOnForwardFailure=yes") != std::string::npos);

    const auto local = tunnel->socketPath();
    tunnel.reset();
    EXPECT(::access(local.c_str(), F_OK) != 0); // The socket and its directory are gone.
    EXPECT(::access(directory.c_str(), F_OK) != 0);
    // The daemon there does not care that we left.
    EXPECT(daemonRunning(fake.dir.path + "/remote.sock"));
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("matterm-daemon --print-socket says where its socket is and starts nothing") {
    const std::string socket = "/tmp/matterm-print-socket-test.sock";
    const std::string command = std::string(MATTERM_DAEMON_BIN) + " --print-socket --daemon-socket " + socket;
    FILE *output = ::popen(command.c_str(), "r");
    EXPECT(output != nullptr);
    std::string line;
    char buffer[256];
    while (output && std::fgets(buffer, sizeof buffer, output)) line += buffer;
    const int status = output ? ::pclose(output) : -1;
    EXPECT_EQ(status, 0);
    EXPECT_EQ(line, socket + "\n");
    EXPECT(::access(socket.c_str(), F_OK) != 0);
}

TEST_CASE("A running remote daemon is reused, not started twice") {
    Fake fake;
    const auto remote = fake.dir.path + "/remote.sock";
    EXPECT_EQ(ensureDaemon(remote, MATTERM_DAEMON_BIN), "");
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    EXPECT(statusOf(tunnel->socketPath()).has_value());
    tunnel.reset();
    stopRemote(remote);
}

TEST_CASE("Failures are reported with the words of ssh") {
    Fake fake;
    std::string error;
    ::setenv("FAKE_SSH_FAIL", "Permission denied (publickey).", 1);
    EXPECT(SshTunnel::open(fake.options(), error) == nullptr);
    EXPECT(error.find("Permission denied (publickey).") != std::string::npos);
    EXPECT(error.find("user@far-away") != std::string::npos);
    ::unsetenv("FAKE_SSH_FAIL");

    ::setenv("FAKE_SSH_NOFORWARD", "bind: Address already in use", 1);
    error.clear();
    EXPECT(SshTunnel::open(fake.options(), error) == nullptr);
    EXPECT(error.find("Address already in use") != std::string::npos);
    ::unsetenv("FAKE_SSH_NOFORWARD");
    stopRemote(fake.dir.path + "/remote.sock");

    auto options = fake.options();
    options.ssh = "/nonexistent/ssh";
    error.clear();
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(error.find("cannot start") != std::string::npos);
}

TEST_CASE("Names that the remote shell or ssh would read are refused before anything runs") {
    Fake fake;
    for (const char *bad : {"-oProxyCommand=evil", "host;rm -rf ~", "a b", "$(x)", "", "host`id`"}) {
        auto options = fake.options();
        options.host = bad;
        std::string error;
        EXPECT(SshTunnel::open(options, error) == nullptr);
        EXPECT(!error.empty());
    }
    for (const char *bad : {"relative.sock", "/a b", "/a;b", "/x$y", "-x"}) {
        auto options = fake.options();
        options.remoteSocket = bad;
        std::string error;
        EXPECT(SshTunnel::open(options, error) == nullptr);
    }
    auto options = fake.options();
    options.remoteDaemon = "matterm-daemon; id";
    std::string error;
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(fileText(fake.log).empty()); // ssh was never run for any of them.
    EXPECT(safeSshWord("user@host.example", false));
    EXPECT(safeSshWord("[::1]", false));
    EXPECT(safeSshWord("/run/user/1000/matterm/daemon.sock", true));
}

TEST_CASE("A tunnel whose ssh ends says so") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    EXPECT(tunnel->alive());
    // Kill what the tunnel started: the user closed the laptop.
    EXPECT_EQ(::kill(tunnel->processId(), SIGTERM), 0);
    EXPECT(until([&] { return !tunnel->alive(); }, 5000));
    tunnel.reset();
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_SUIT_END
