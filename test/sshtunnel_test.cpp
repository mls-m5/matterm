// The daemon of "another computer" (here: another socket) reached through SshTunnel, with a fake ssh
// (test/fakessh.py) that relays Unix sockets like `ssh -L local:remote` does. The real matterm-daemon runs
// behind it, started by `matterm-daemon --ensure --print-socket` exactly as the tunnel does over ssh.
#include "daemonclient.h"
#include "daemonlink.h"
#include "mls-unit-test/unittest.h"
#include "sshtunnel.h"

#include <algorithm>
#include <chrono>
#include <dirent.h>
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

// MATTERM_TEST_DAEMON: run the tests against another build of the daemon (the static one).
std::string daemonBinary() {
    const char *other = std::getenv("MATTERM_TEST_DAEMON");
    return other && *other ? other : MATTERM_DAEMON_BIN;
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
        ::setenv("FAKE_SSH_DAEMON", daemonBinary().c_str(), 1);
        ::setenv("FAKE_SSH_DAEMON_ON_PATH", "1", 1);
        ::setenv("FAKE_SSH_HOME", (dir.path + "/home").c_str(), 1);
        ::unsetenv("FAKE_SSH_UNAME");
        ::unsetenv("FAKE_SSH_FAIL");
        ::unsetenv("FAKE_SSH_NOFORWARD");
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("PS1", "$ ", 1);
    }
    ~Fake() {
        ::unsetenv("FAKE_SSH_LOG");
        ::unsetenv("FAKE_SSH_FAIL");
        ::unsetenv("FAKE_SSH_NOFORWARD");
        ::unsetenv("FAKE_SSH_UNAME");
        ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
        ::unsetenv("FAKE_SSH_HOME");
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

// The installed copies under the fake HOME.
std::vector<std::string> installed(const Fake &fake) {
    std::vector<std::string> names;
    const auto directory = fake.dir.path + "/home/.local/share/matterm/bin";
    if (auto *d = ::opendir(directory.c_str())) {
        while (const auto *entry = ::readdir(d)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

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

TEST_CASE("sha256 is the real one") {
    frontend::Sha256 empty;
    EXPECT_EQ(empty.hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    frontend::Sha256 abc;
    abc.update("abc");
    EXPECT_EQ(abc.hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    frontend::Sha256 split; // Across the block boundary, fed in pieces.
    const std::string text(1000, 'a');
    for (std::size_t i = 0; i < text.size(); i += 7) split.update(text.substr(i, 7));
    frontend::Sha256 whole;
    whole.update(text);
    EXPECT_EQ(split.hex(), whole.hex());
    frontend::Sha256 million;
    for (int i = 0; i < 10000; ++i) million.update(std::string(100, 'a'));
    EXPECT_EQ(million.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("isStaticElf tells a static binary from a dynamic one and from other files") {
    EXPECT(!isStaticElf("/nonexistent"));
    EXPECT(!isStaticElf(FAKE_SSH)); // A script.
    EXPECT(!isStaticElf("/bin/sh")); // Dynamic.
    if (const char *other = std::getenv("MATTERM_TEST_DAEMON"); other && *other) EXPECT(isStaticElf(other));
}

TEST_CASE("--daemon-install: our daemon is copied (after asking), named after its content, and started there") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    auto options = fake.options();
    options.install = DaemonInstall::Always;
    options.localDaemon = daemonBinary();
    std::vector<std::string> asked, said;
    options.confirm = [&](const std::string &q) {
        asked.push_back(q);
        return true;
    };
    options.progress = [&](const std::string &l) { said.push_back(l); };
    std::string error;
    auto tunnel = SshTunnel::open(options, error);
    EXPECT(tunnel != nullptr);
    EXPECT_EQ(error, "");
    EXPECT_EQ(asked.size(), std::size_t(1));
    EXPECT(asked[0].find("Copy ours") != std::string::npos);
    const auto names = installed(fake);
    EXPECT_EQ(names.size(), std::size_t(1));
    EXPECT(names[0].rfind("matterm-daemon-", 0) == 0);
    EXPECT_EQ(names[0].size(), std::string("matterm-daemon-").size() + 24);
    // The copy is mode 0700, the same bytes, and it is the one that runs.
    const auto file = fake.dir.path + "/home/.local/share/matterm/bin/" + names[0];
    struct stat st {};
    EXPECT_EQ(::stat(file.c_str(), &st), 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0700);
    EXPECT(statusOf(tunnel->socketPath()).has_value());
    EXPECT(fileText(fake.log).find(names[0]) != std::string::npos);
    tunnel.reset();
    // Again: present, so nothing is asked and nothing is copied; the running daemon is reused.
    asked.clear();
    auto again = SshTunnel::open(options, error);
    EXPECT(again != nullptr);
    EXPECT(asked.empty());
    EXPECT_EQ(installed(fake).size(), std::size_t(1));
    again.reset();
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("Nothing is copied without a yes; --yes copies without asking") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    auto options = fake.options();
    options.install = DaemonInstall::Always;
    options.localDaemon = daemonBinary();
    std::string error;
    EXPECT(SshTunnel::open(options, error) == nullptr); // No confirm callback: no.
    EXPECT(error.find("not confirmed") != std::string::npos);
    options.confirm = [](const std::string &) { return false; };
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(installed(fake).empty()); // Not even the temporary file is left.
    const auto directory = fake.dir.path + "/home/.local/share/matterm/bin";
    options.confirm = nullptr;
    options.assumeYes = true;
    auto tunnel = SshTunnel::open(options, error);
    EXPECT(tunnel != nullptr);
    EXPECT_EQ(installed(fake).size(), std::size_t(1));
    tunnel.reset();
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("Auto: the daemon on PATH is used, and ours is offered only when there is none") {
    Fake fake;
    auto options = fake.options();
    options.install = DaemonInstall::Auto;
    options.localDaemon = daemonBinary();
    bool asked = false;
    options.confirm = [&](const std::string &) {
        asked = true;
        return true;
    };
    std::string error;
    auto onPath = SshTunnel::open(options, error); // matterm-daemon is on the fake's PATH.
    EXPECT(onPath != nullptr);
    EXPECT(!asked);
    EXPECT(installed(fake).empty());
    onPath.reset();
    stopRemote(fake.dir.path + "/remote.sock");
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH"); // Now it is not.
    auto copied = SshTunnel::open(options, error);
    EXPECT(copied != nullptr);
    EXPECT(asked);
    EXPECT_EQ(installed(fake).size(), std::size_t(1));
    copied.reset();
    stopRemote(fake.dir.path + "/remote.sock");
    // Never: a missing daemon is an error that says what to do.
    options.install = DaemonInstall::Never;
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(error.find("--daemon-install") != std::string::npos);
}

TEST_CASE("Another architecture is a plain error, and nothing is copied") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    ::setenv("FAKE_SSH_UNAME", "Linux aarch64", 1);
    auto options = fake.options();
    options.install = DaemonInstall::Always;
    options.assumeYes = true;
    options.localDaemon = daemonBinary();
    std::string error;
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(error.find("aarch64") != std::string::npos);
    EXPECT(error.find("Build matterm-daemon on") != std::string::npos);
    EXPECT(installed(fake).empty());
    ::setenv("FAKE_SSH_UNAME", "Darwin arm64", 1);
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(error.find("Darwin") != std::string::npos);
}

TEST_CASE("A file that does not run there is not installed") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    const auto notDaemon = fake.dir.path + "/not-a-daemon";
    {
        std::ofstream out(notDaemon);
        out << "#!/bin/sh\nexit 3\n";
    }
    ::chmod(notDaemon.c_str(), 0700);
    auto options = fake.options();
    options.install = DaemonInstall::Always;
    options.assumeYes = true;
    options.localDaemon = notDaemon;
    std::string error;
    EXPECT(SshTunnel::open(options, error) == nullptr);
    EXPECT(error.find("failed") != std::string::npos);
    EXPECT(installed(fake).empty()); // No half file under the final name, no leftover temporary file.
    const auto leftovers = fake.dir.path + "/home/.local/share/matterm/bin";
    EXPECT(::access(leftovers.c_str(), F_OK) == 0);
}

TEST_CASE("The install directory must be a safe absolute path") {
    Fake fake;
    for (const char *bad : {"relative/dir", "/a b", "/a;rm -rf x", "/x$HOME"}) {
        auto options = fake.options();
        options.install = DaemonInstall::Always;
        options.assumeYes = true;
        options.localDaemon = daemonBinary();
        options.installDirectory = bad;
        std::string error;
        EXPECT(SshTunnel::open(options, error) == nullptr);
    }
}

TEST_SUIT_END
