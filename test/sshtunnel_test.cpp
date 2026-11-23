// The daemon of "another computer" (here: another socket) reached through SshTunnel, with a fake ssh
// (test/fakessh.py) that relays Unix sockets like `ssh -L local:remote` does. The real matterm-daemon runs
// behind it, started by `matterm-daemon --ensure --print-socket` exactly as the tunnel does over ssh.
#include "daemonclient.h"
#include "daemoninstall.h"
#include "daemonlink.h"
#include "mls-unit-test/unittest.h"
#include "sshtunnel.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
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

// MATTERM_TEST_DAEMON: run the tests against another build of the daemon (the
// static one). The name a program is installed under: its version and the first
// 8 of its hash; a build with uncommitted changes (or none from git) is named
// by its content instead.
std::string expectedInstallName(const std::string &program,
                                const std::string &file) {
    auto version = mtd::ownVersion();
    if (version.hash.size() < 8 || version.hash == "unknown" ||
        version.hash.find("dirty") != std::string::npos) {
        std::ifstream in(file, std::ios::binary);
        Sha256 sha;
        char buffer[65536];
        while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
            sha.update(buffer, std::size_t(in.gcount()));
        }
        version.hash = sha.hex();
    }
    return installedName(program, version);
}

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
        // NOLINTNEXTLINE(cert-env33-c): cleanup of our own directory
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
        ::unsetenv("FAKE_SSH_DELAY");
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("PS1", "$ ", 1);
    }
    ~Fake() {
        ::unsetenv("FAKE_SSH_LOG");
        ::unsetenv("FAKE_SSH_FAIL");
        ::unsetenv("FAKE_SSH_NOFORWARD");
        ::unsetenv("FAKE_SSH_DELAY");
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
            // The versioned daemon files only (not matterm-attach, not the
            // stable links).
            if (name.rfind("matterm-daemon-", 0) == 0)
                names.push_back(name);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

namespace {
std::uint64_t nowMs() {
    return std::uint64_t(
        std::chrono::duration_cast<milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Ticks until `event` comes (returns true) or the time is up.
bool untilEvent(TunnelReconnector &reconnector,
                TunnelReconnector::Event event,
                int timeoutMs = 20000) {
    return until(
        [&] {
            const auto got = reconnector.tick(nowMs());
            return got == event;
        },
        timeoutMs);
}

// The numbers n of every "L<n>" word on the screen and in the history, in
// order.
std::vector<int> numbersOf(TerminalView &view) {
    std::vector<int> numbers;
    std::istringstream in(textOf(view));
    std::string word;
    while (in >> word) {
        if (word.size() > 1 && word[0] == 'L' &&
            word.find_first_not_of("0123456789", 1) == std::string::npos) {
            numbers.push_back(std::stoi(word.substr(1)));
        }
    }
    return numbers;
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
    link->close(); // Ends the terminal there so that stopRemote below can stop
                   // the daemon.
    ::usleep(200000);

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
    // NOLINTNEXTLINE(cert-env33-c): runs our own daemon binary
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
    EXPECT_EQ(
        names[0],
        expectedInstallName("matterm-daemon",
                            daemonBinary())); // matterm-daemon-1.3.1-<hash8>
    EXPECT(parseInstalledName(names[0]).has_value());
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

TEST_CASE("matterm-attach is copied with the daemon, and old versions of both "
          "are removed") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    const auto directory = fake.dir.path + "/home/.local/share/matterm/bin";
    std::filesystem::create_directories(directory);
    // Left from earlier installs: an old daemon and attach, an older fallback,
    // a newer one, one that runs (a copy of sleep), and files that are not
    // ours.
    const auto put = [&](const std::string &name,
                         const std::string &text = "x") {
        std::ofstream(directory + "/" + name) << text;
    };
    put("matterm-daemon-1.2.0-aaaaaaa1");
    put("matterm-daemon-1.1.0-aaaaaaa2");
    put("matterm-daemon-1.0.0-aaaaaaa3");
    put("matterm-attach-1.1.0-aaaaaaa4");
    put("matterm-daemon-9.9.9-aaaaaaa5");
    put("matterm-daemon-0123456789abcdef01234567");
    put("notes.txt");
    auto options = fake.options();
    options.install = DaemonInstall::Always;
    options.assumeYes = true;
    options.localDaemon = daemonBinary();
    std::vector<std::string> said;
    options.progress = [&](const std::string &l) { said.push_back(l); };
    std::string error;
    auto tunnel = SshTunnel::open(options, error);
    EXPECT(tunnel != nullptr);
    std::vector<std::string> names;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    const auto own = mtd::ownVersion();
    const auto has = [&](const std::string &n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    EXPECT(has(expectedInstallName("matterm-daemon", daemonBinary())));
    EXPECT(has(expectedInstallName(
        "matterm-attach",
        daemonBinary().substr(0, daemonBinary().rfind('/') + 1) +
            "matterm-attach")));
    EXPECT(has("matterm-daemon-1.2.0-aaaaaaa1")); // The newest older one stays.
    EXPECT(!has("matterm-daemon-1.1.0-aaaaaaa2"));
    EXPECT(!has("matterm-daemon-1.0.0-aaaaaaa3"));
    EXPECT(has("matterm-attach-1.1.0-aaaaaaa4")); // The only older attach: the
                                                  // fallback.
    EXPECT(has("matterm-daemon-9.9.9-aaaaaaa5")); // Newer: left alone.
    EXPECT(has("matterm-daemon-0123456789abcdef01234567")); // Not our naming
                                                            // now: left alone.
    EXPECT(has("notes.txt"));
    bool reported = false;
    for (const auto &line : said) {
        reported = reported ||
                   line.find("removed the old matterm-daemon-1.1.0-aaaaaaa2") !=
                       std::string::npos;
    }
    EXPECT(reported);
    tunnel.reset();
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("matterm-attach on the other computer: installed on demand, stable "
          "names, list and stop") {
    Fake fake;
    ::unsetenv("FAKE_SSH_DAEMON_ON_PATH");
    auto options = fake.options();
    options.install = DaemonInstall::Never;
    options.assumeYes = true;
    options.localDaemon = daemonBinary();
    std::string error;
    EXPECT_EQ(SshTunnel::runRemoteAttach(options, {"--version"}, error),
              -1); // Not there, not allowed to copy.
    EXPECT(error.find("--daemon-install") != std::string::npos);
    options.install = DaemonInstall::Auto; // Copies ours, with the daemon.
    auto tunnel = SshTunnel::open(options, error);
    EXPECT(tunnel != nullptr);
    tunnel.reset();
    const auto directory = fake.dir.path + "/home/.local/share/matterm/bin";
    // The stable names point at the installed versions.
    struct stat st {};
    EXPECT_EQ(::lstat((directory + "/matterm-attach").c_str(), &st), 0);
    EXPECT(S_ISLNK(st.st_mode));
    EXPECT_EQ(::lstat((directory + "/matterm-daemon").c_str(), &st), 0);
    EXPECT(S_ISLNK(st.st_mode));
    const auto out = fake.dir.path + "/out.txt";
    const auto run = [&](const std::vector<std::string> &args) {
        const int saved = ::dup(1);
        const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(fd, 1);
        ::close(fd);
        const int code = SshTunnel::runRemoteAttach(options, args, error);
        (void)::fflush(stdout);
        ::dup2(saved, 1);
        ::close(saved);
        return code;
    };
    EXPECT_EQ(run({"--version"}), 0);
    EXPECT(fileText(out).find("matterm-attach " +
                              mtd::toString(mtd::ownVersion())) !=
           std::string::npos);
    // The daemon on the other computer is listed there, then stopped (nothing
    // to ask on: refused).
    auto again = SshTunnel::open(options, error);
    EXPECT(again != nullptr);
    EXPECT_EQ(run({"--daemon-list", "--daemon-socket", options.remoteSocket}),
              0);
    EXPECT(fileText(out).find(options.remoteSocket) != std::string::npos);
    EXPECT(fileText(out).find("terminals") != std::string::npos);
    EXPECT(statusOf(again->socketPath()).has_value());
    EXPECT_EQ(run({"--daemon-stop", "--daemon-socket", options.remoteSocket}),
              1);
    EXPECT(fileText(out).find("Not stopped") != std::string::npos);
    EXPECT(statusOf(again->socketPath()).has_value());
    again.reset();
    stopRemote(options.remoteSocket);
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

TEST_CASE("Reconnect: ssh is killed while a program prints, the pane attaches "
          "again and no byte is lost") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    TunnelReconnector reconnector(*tunnel, ReconnectPolicy{{0, 20}, 5});
    const auto socket = tunnel->socketPath();

    auto view = TerminalView(TerminalView::Config{60, 10});
    auto link = attachToDaemon(view, socket, "", 0, clockNow);
    EXPECT(link != nullptr);
    EXPECT(until([&] {
        link->tick();
        return !link->sessionId().empty();
    }));
    EXPECT(view.sendInput("i=0; while [ $i -lt 80 ]; do echo L$i; i=$((i+1)); "
                          "sleep 0.05; done\n"));
    EXPECT(until([&] {
        link->tick();
        const auto n = numbersOf(view);
        return !n.empty() && n.back() >= 5;
    }));
    EXPECT(reconnector.tick(nowMs()) == TunnelReconnector::Event::None);
    EXPECT(reconnector.state() == TunnelReconnector::State::Connected);

    // The laptop goes to sleep: ssh is gone, the program goes on there.
    const auto oldProcess = tunnel->processId();
    EXPECT_EQ(::kill(oldProcess, SIGKILL), 0);
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::Lost, 5000));
    EXPECT(until([&] {
        link->tick();
        return view.state() == TerminalView::State::Disconnected;
    }));
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::Reconnected));
    EXPECT(reconnector.state() == TunnelReconnector::State::Connected);
    EXPECT(tunnel->alive());
    EXPECT(tunnel->processId() != oldProcess);
    EXPECT_EQ(tunnel->socketPath(),
              socket); // Same place: the link finds it as before.

    auto fresh = reattachToDaemon(view, link.get(), socket, clockNow);
    EXPECT(fresh != nullptr);
    link = std::move(fresh);
    EXPECT(until([&] {
        link->tick();
        const auto n = numbersOf(view);
        return !n.empty() && n.back() == 79;
    }));
    // Every line exactly once, in order: replayed from the offset, not from the
    // start, not skipped.
    const auto numbers = numbersOf(view);
    EXPECT_EQ(numbers.size(), std::size_t(80));
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        EXPECT_EQ(numbers[i], int(i));
    }
    view.detach();
    link->close(); // The terminal ends there, or the daemon would refuse to
                   // stop.
    ::usleep(200000);
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE(
    "Reconnect: the attempts back off, give up, and the user's retry goes on") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    ReconnectPolicy policy;
    policy.delaysMs = {0, 150, 300};
    policy.maxAttempts = 3;
    TunnelReconnector reconnector(*tunnel, policy);

    ::setenv("FAKE_SSH_FAIL",
             "ssh: connect to host far-away port 22: Network is unreachable",
             1);
    EXPECT_EQ(::kill(tunnel->processId(), SIGKILL), 0);
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::Lost, 5000));
    EXPECT(reconnector.dueMs().has_value());

    // Failed attempts are 0 ms, 150 ms and 300 ms apart: at least that long all
    // together.
    const auto began = std::chrono::steady_clock::now();
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::GaveUp, 10000));
    const auto took = std::chrono::duration_cast<milliseconds>(
        std::chrono::steady_clock::now() - began);
    EXPECT(took.count() >= 400);
    EXPECT(reconnector.state() == TunnelReconnector::State::GivenUp);
    EXPECT_EQ(reconnector.attempt(), 3);
    EXPECT(reconnector.lastError().find("Network is unreachable") !=
           std::string::npos);
    EXPECT(!reconnector.dueMs().has_value());
    // Given up means quiet: nothing is tried by itself.
    ::usleep(300000);
    EXPECT(reconnector.tick(nowMs()) == TunnelReconnector::Event::None);
    EXPECT(reconnector.state() == TunnelReconnector::State::GivenUp);

    // The link is back; Reconnect from the palette (or Enter in the pane).
    ::unsetenv("FAKE_SSH_FAIL");
    EXPECT(reconnector.retryNow(nowMs()));
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::Reconnected));
    EXPECT_EQ(reconnector.attempt(), 0);
    EXPECT(tunnel->alive());
    EXPECT(daemonRunning(tunnel->socketPath()));
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("Reconnect never blocks the caller, however slow the link is") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    TunnelReconnector reconnector(*tunnel, ReconnectPolicy{{0}, 3});
    ::setenv("FAKE_SSH_DELAY", "1.5", 1); // Every ssh call takes 1.5 s.
    EXPECT_EQ(::kill(tunnel->processId(), SIGKILL), 0);
    long long slowest = 0;
    bool sawConnecting = false;
    bool reconnected = false;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!reconnected && std::chrono::steady_clock::now() < end) {
        const auto before = std::chrono::steady_clock::now();
        const auto event = reconnector.tick(nowMs());
        const bool alive = tunnel->alive(); // Asked from here while the worker
                                            // starts and stops processes.
        (void)alive;
        slowest =
            std::max<long long>(slowest,
                                std::chrono::duration_cast<milliseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
        sawConnecting =
            sawConnecting ||
            reconnector.state() == TunnelReconnector::State::Connecting;
        reconnected = event == TunnelReconnector::Event::Reconnected;
        std::this_thread::sleep_for(milliseconds(5));
    }
    EXPECT(reconnected);
    EXPECT(sawConnecting);
    EXPECT(slowest < 200);
    ::unsetenv("FAKE_SSH_DELAY");
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE("Reconnect: retryNow replaces an ssh that is alive but stuck") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    TunnelReconnector reconnector(*tunnel, ReconnectPolicy{{0}, 3});
    const auto stuck = tunnel->processId();
    EXPECT(reconnector.retryNow(nowMs()));
    EXPECT(!reconnector.retryNow(nowMs())); // One attempt at a time.
    EXPECT(untilEvent(reconnector, TunnelReconnector::Event::Reconnected));
    EXPECT(tunnel->processId() != stuck);
    EXPECT(daemonRunning(tunnel->socketPath()));
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_CASE(
    "Closing the window during a reconnect does not wait for the attempt") {
    Fake fake;
    std::string error;
    auto tunnel = SshTunnel::open(fake.options(), error);
    EXPECT(tunnel != nullptr);
    const auto began = std::chrono::steady_clock::now();
    {
        TunnelReconnector reconnector(*tunnel, ReconnectPolicy{{0}, 3});
        ::setenv("FAKE_SSH_DELAY", "15", 1);
        EXPECT(reconnector.retryNow(nowMs()));
        ::usleep(300000);
    } // The destructor cancels the ssh call.
    const auto took = std::chrono::duration_cast<milliseconds>(
        std::chrono::steady_clock::now() - began);
    EXPECT(took.count() < 5000);
    ::unsetenv("FAKE_SSH_DELAY");
    tunnel.reset();
    stopRemote(fake.dir.path + "/remote.sock");
}

TEST_SUIT_END
