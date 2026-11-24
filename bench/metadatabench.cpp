// Detached flood, no list vs metadata off/on. Uses a SEPARATE daemon process
// and its existing performance log's SELF CPU, excluding flood-program CPU.
// Never prints terminal contents. No attach/takeover or matperf dependency.
#include "daemonclient.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
struct Child {
    std::string dir, socket, log;
    pid_t pid = -1;
    Child() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "matterm-metadata-XXXXXX")
                .string();
        const auto made = ::mkdtemp(pattern.data());
        if (!made)
            throw std::runtime_error("cannot create benchmark directory");
        dir = made;
        socket = dir + "/daemon.sock";
        log = dir + "/perf.log";
    }
    void stop() {
        if (pid <= 0)
            return;
        (void)::kill(pid, SIGTERM);
        int status = 0;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < until) {
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno != EINTR)) {
                pid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)::kill(pid, SIGKILL); // Only this benchmark's own child.
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        pid = -1;
    }
    ~Child() {
        stop();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    void start(const std::string &program) {
        pid = ::fork();
        if (pid < 0)
            throw std::runtime_error("fork failed");
        if (pid == 0) {
            ::execl(program.c_str(),
                    program.c_str(),
                    "--daemon-socket",
                    socket.c_str(),
                    "--idle-seconds",
                    "0",
                    "--perf-log",
                    log.c_str(),
                    static_cast<char *>(nullptr));
            ::_exit(127);
        }
        const auto until = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < until) {
            if (::access(socket.c_str(), F_OK) == 0)
                return;
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                throw std::runtime_error("daemon exited at startup");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("daemon socket did not appear");
    }
};
mtd::ControlReply ask(const std::string &socket,
                      const mtd::ControlRequest &request) {
    std::string error;
    const auto reply = frontend::askDaemon(socket, request, error, 5000);
    if (!reply)
        throw std::runtime_error("daemon request: " + error);
    if (std::holds_alternative<mtd::ControlError>(*reply))
        throw std::runtime_error("daemon rejected benchmark request");
    return *reply;
}
std::uint64_t bytes(const std::string &socket, std::size_t expected) {
    const auto reply = ask(socket, mtd::ListSessions{});
    const auto *list = std::get_if<mtd::SessionList>(&reply);
    if (!list || list->sessions.size() != expected)
        throw std::runtime_error("flood program failed (python3 required)");
    std::uint64_t total = 0;
    for (const auto &s : list->sessions) {
        if (s.attached)
            throw std::runtime_error("benchmark must not attach a terminal");
        total += s.outputEnd;
    }
    return total;
}
double field(const std::string &line, const std::string &key) {
    const auto at = line.find(" " + key + "=");
    if (at == std::string::npos)
        throw std::runtime_error("missing log metric: " + key);
    return std::stod(line.substr(at + key.size() + 2));
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 6)
            throw std::invalid_argument(
                "usage: matterm-metadata-bench DAEMON [none|off|on] "
                "[seconds=5] [sessions=4] [text|string|rep|ansi]");
        const std::string program = std::filesystem::absolute(argv[1]).string();
        const std::string mode = argc > 2 ? argv[2] : "on";
        const int duration = argc > 3 ? std::stoi(argv[3]) : 5;
        const int sessions = argc > 4 ? std::stoi(argv[4]) : 4;
        if ((mode != "none" && mode != "off" && mode != "on") || duration < 1 ||
            duration > 60 || sessions < 1 || sessions > 16)
            throw std::invalid_argument(
                "invalid mode, duration (1..60) or sessions (1..16)");
        const std::string workload = argc > 5 ? argv[5] : "text";
        std::string packet;
        if (workload == "text")
            packet = "b'\\x1b\\\\\\x1b[32m'+b'x'*4000+b'\\r\\n'";
        else if (workload == "string")
            packet = "b'\\x1b]52;'+b's'*4000+b'\\x1b\\\\ok\\r\\n'";
        else if (workload == "rep")
            packet = "b'\\x1b\\\\R\\x1b[2147483647b\\r\\n'*200";
        else if (workload == "ansi")
            packet = "b'\\x1b[32m'+b'x'*4000+b'\\r\\n'";
        else
            throw std::invalid_argument("unknown workload");
        Child child;
        child.start(program);
        std::vector<std::string> ids;
        // ST makes a cold tail explicitly synchronizable; strings/SGR/CRLF
        // mimic editor output. Pty backpressure bounds this producer naturally.
        const std::string flood =
            "import os,time\np=" + packet +
            "\nwhile True:\n os.write(1,p)\n time.sleep(0.002)";
        for (int i = 0; i < sessions; ++i) {
            const auto reply = ask(
                child.socket,
                mtd::CreateTerminal{"", 80, 24, {"python3", "-c", flood}, ""});
            const auto *created = std::get_if<mtd::SessionCreated>(&reply);
            if (!created)
                throw std::runtime_error("failed to create flood program");
            ids.push_back(created->id);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Startup warmup.
        const auto before = bytes(child.socket, ids.size());
        const auto began = Clock::now(),
                   until = began + std::chrono::seconds(duration);
        auto next = began;
        std::size_t requests = 0, known = 0, unavailable = 0, nextId = 0;
        while (Clock::now() < until) {
            if (Clock::now() >= next) {
                if (mode != "none") {
                    std::vector<std::string> ordered;
                    for (std::size_t i = 0; i < ids.size(); ++i)
                        ordered.push_back(ids[(nextId + i) % ids.size()]);
                    const auto reply =
                        ask(child.socket,
                            mtd::GetSessionDetails{ordered, mode == "on"});
                    const auto *details =
                        std::get_if<mtd::SessionDetails>(&reply);
                    if (!details || details->processedCount == 0 ||
                        details->processedCount > ordered.size())
                        throw std::runtime_error(
                            "details made no valid progress");
                    nextId = (nextId + details->processedCount) % ids.size();
                    for (const auto &r : details->rows) {
                        if (r.preview)
                            ++known;
                        else
                            ++unavailable;
                        if (mode == "off" && r.preview)
                            throw std::runtime_error(
                                "preview leaked while off");
                    }
                    ++requests;
                }
                next = Clock::now() + std::chrono::seconds(
                                          1); // Never catch up in a busy loop.
            }
            std::this_thread::sleep_until(std::min(next, until));
        }
        const double wall =
            std::chrono::duration<double>(Clock::now() - began).count();
        const auto received = bytes(child.socket, ids.size()) - before;
        child.stop();
        std::ifstream log(child.log);
        std::string line, summary;
        while (std::getline(log, line))
            if (line.find(" summary ") != std::string::npos)
                summary = line;
        if (summary.empty())
            throw std::runtime_error("daemon log summary missing");
        std::cout << "mode=" << mode << " workload=" << workload
                  << " sessions=" << sessions << " measured_s=" << wall
                  << " pty_MiB_s=" << (double(received) / (1024 * 1024)) / wall
                  << " daemon_self_cpu_ms=" << field(summary, "cpu_ms")
                  << " daemon_log_interval_s=" << field(summary, "interval_s")
                  << " daemon_cpu_percent=" << field(summary, "cpu")
                  << " details_requests=" << requests
                  << " preview_known_rows=" << known
                  << " preview_unavailable_rows=" << unavailable << '\n';
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
