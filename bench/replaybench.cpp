// matterm-replay-bench: where the time goes when a client reattaches to a
// daemon terminal whose replay ring is full (docs/CPU.md "Replay"). An
// in-process daemon runs a terminal that printed MIB mebibytes; a client
// attaches (the way the window and matmux do: DaemonLink + TerminalView) and
// the program reports, for the replay: wall time, bytes, bytes per round of the
// client's loop, time in the parser and model, time in everything else
// (transport, framing, acks). The window loop's cadence is simulated with
// --round-ms (0: as fast as it goes) and, with --frame, the cost of looking at
// the frame every round (the signature hash a window computes) is added. With
// --restore K instead: how long K panes take to come up (K terminals made and
// attached, one after the other as a session restore does), measured to the
// first output of the last one. usage: matterm-replay-bench [--mib N]
// [--round-ms M] [--frame] [--columns C] [--rows R] [--restore K]
#include "daemon.h"
#include "daemonclient.h"
#include "daemonlink.h"
#include "framesignature.h"
#include "terminalview.h"
#include "tty/matterm/snapshot.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}
protocol::Time clockNow() {
    return std::chrono::duration_cast<protocol::Time>(
        Clock::now().time_since_epoch());
}
} // namespace

int main(int argc, char **argv) {
    int mib = 4, roundMs = 0, columns = 100, rows = 40, restore = 0;
    bool frame = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mib" && i + 1 < argc)
            mib = std::atoi(argv[++i]);
        else if (a == "--round-ms" && i + 1 < argc)
            roundMs = std::atoi(argv[++i]);
        else if (a == "--columns" && i + 1 < argc)
            columns = std::atoi(argv[++i]);
        else if (a == "--rows" && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (a == "--frame")
            frame = true;
        else if (a == "--restore" && i + 1 < argc)
            restore = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: matterm-replay-bench [--mib N] [--round-ms M] "
                         "[--frame] [--columns C] [--rows R]\n";
            return 2;
        }
    }
    std::string temp =
        (std::filesystem::temp_directory_path() / "mtrbXXXXXX").string();
    const std::string dir = ::mkdtemp(temp.data());
    const std::string socket = dir + "/d.sock";

    auto config = mtd::defaultConfig();
    config.socketPath = socket;
    config.privateDirectory = false;
    config.idleExitSeconds = 0;
    config.registry.replayBufferBytes = std::size_t(mib + 1)
                                        << 20; // The ring holds all of it.
    mtd::Daemon daemon(std::move(config));
    std::string error;
    if (!daemon.start(error)) {
        std::cerr << "daemon: " << error << "\n";
        return 1;
    }
    std::thread server([&] { daemon.run(); });

    if (restore > 0) {
        frontend::TerminalViewConfig viewConfig;
        viewConfig.columns = std::size_t(columns);
        viewConfig.rows = std::size_t(rows);
        std::vector<std::unique_ptr<frontend::TerminalView>> views;
        std::vector<std::unique_ptr<frontend::DaemonLink>> links;
        const auto begin = Clock::now();
        double createTime = 0;
        for (int i = 0; i < restore; ++i) {
            mtd::CreateTerminal one;
            one.columns = std::uint16_t(columns);
            one.rows = std::uint16_t(rows);
            one.command = {"/bin/sh", "-c", "echo ready; sleep 600"};
            const auto t0 = Clock::now();
            const auto made = frontend::createTerminal(socket, one, error);
            createTime += seconds(Clock::now() - t0);
            if (!made) {
                std::cerr << "create: " << error << "\n";
                return 1;
            }
            if (std::getenv("BENCH_WAIT_MS")) { // Attach after the output
                                                // exists: replay latency alone.
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::atoi(std::getenv("BENCH_WAIT_MS"))));
            }
            views.push_back(
                std::make_unique<frontend::TerminalView>(viewConfig));
            links.push_back(frontend::attachToDaemon(
                *views.back(), socket, *made, 0, clockNow));
        }
        const auto created = Clock::now();
        auto allReady = [&] {
            for (auto &v : views) {
                if (v->outputBytes() == 0)
                    return false;
            }
            return true;
        };
        while (!allReady() && seconds(Clock::now() - begin) < 60) {
            for (auto &l : links)
                l->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << restore << " panes: all made in "
                  << seconds(created - begin)
                  << " s (of which asking the daemon " << createTime
                  << " s), all showing output after "
                  << seconds(Clock::now() - begin) << " s\n";
        for (auto &v : views)
            v->detach();
        links.clear();
        std::string e;
        (void)frontend::askDaemon(
            socket, mtd::Shutdown(std::uint32_t(restore)), e);
        daemon.stop();
        server.join();
        std::filesystem::remove_all(dir);
        return 0;
    }

    // A terminal that prints about MIB mebibytes of coloured log lines and then
    // waits.
    const long lines = long(mib) * 1048576L / 95;
    mtd::CreateTerminal create;
    create.columns = std::uint16_t(columns);
    create.rows = std::uint16_t(rows);
    create.command = {
        "/bin/sh",
        "-c",
        "seq 1 " + std::to_string(lines) +
            " | sed 's/.*/\\x1b[32mline & of the log\\x1b[0m the quick brown "
            "fox jumps over the lazy dog/'; sleep 600"};
    const auto id = frontend::createTerminal(socket, create, error);
    if (!id) {
        std::cerr << "create: " << error << "\n";
        return 1;
    }
    // Wait until the ring holds what the program printed.
    std::uint64_t ringEnd = 0, last = 1;
    for (int i = 0; i < 400 && (ringEnd == 0 || ringEnd != last); ++i) {
        last = ringEnd;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string e;
        const auto reply = frontend::askDaemon(socket, mtd::ListSessions{}, e);
        if (reply && std::holds_alternative<mtd::SessionList>(*reply)) {
            for (const auto &s : std::get<mtd::SessionList>(*reply).sessions) {
                if (s.id == *id)
                    ringEnd = s.outputEnd;
            }
        }
    }
    std::cout << "ring: " << ringEnd << " bytes (" << double(ringEnd) / 1048576
              << " MiB)\n";

    // The client: attach from the start of the ring.
    frontend::TerminalViewConfig viewConfig;
    viewConfig.columns = std::size_t(columns);
    viewConfig.rows = std::size_t(rows);
    viewConfig.scrollback = 10000;
    frontend::TerminalView view(viewConfig);
    const auto start = Clock::now();
    auto link = frontend::attachToDaemon(view, socket, *id, 0, clockNow);
    if (!link) {
        std::cerr << "attach failed\n";
        return 1;
    }
    long rounds = 0;
    double busy = 0, signature = 0;
    std::size_t before = 0;
    std::uint64_t hashSum = 0;
    while (link->receivedOffset() < ringEnd &&
           seconds(Clock::now() - start) < 120) {
        const auto t0 = Clock::now();
        link->tick();
        ++rounds;
        if (frame) {
            const auto t1 = Clock::now();
            // What a window asks each round: did the frame change? (hash of
            // every cell)
            const auto &model = view.model();
            frontend::FrameSignature s;
            for (std::size_t y = 0; y < model.rows(); ++y) {
                for (std::size_t x = 0; x < model.columns(); ++x) {
                    const auto &cell = model.cell(x, y);
                    s.bytes(&cell, sizeof cell);
                }
            }
            hashSum += s.value();
            signature += seconds(Clock::now() - t1);
        }
        busy += seconds(Clock::now() - t0);
        if (roundMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(roundMs));
        }
        else {
            std::this_thread::sleep_for(
                std::chrono::microseconds(50)); // Let the daemon thread run.
        }
    }
    const double wall = seconds(Clock::now() - start);
    const std::size_t bytes = std::size_t(link->receivedOffset());
    (void)before;
    std::cout << "replayed " << bytes << " bytes in " << wall << " s ("
              << double(bytes) / 1048576 / wall << " MiB/s)\n"
              << "rounds: " << rounds << " ("
              << (rounds ? bytes / std::size_t(rounds) : 0)
              << " bytes per round)\n"
              << "time in client tick (transport + parse + model): "
              << busy - signature << " s\n";
    if (frame) {
        std::cout << "time hashing the frame each round: " << signature
                  << " s (sum " << hashSum << ")\n";
    }
    {
        // The passthrough snapshot of what was just replayed
        // (src/tty/matterm/snapshot.h).
        const auto t0 = Clock::now();
        const auto bytesOut = tty::snapshot(view.model(), view.modes());
        std::cout << "snapshot of the model (" << view.model().historySize()
                  << " saved rows + screen): " << bytesOut.size()
                  << " bytes in " << seconds(Clock::now() - t0) * 1000
                  << " ms\n";
    }
    std::cout << "waiting for the daemon / sleeping: " << wall - busy << " s\n";
    view.detach(); // Before the link goes (the view listens to it).
    link.reset();
    std::string e;
    (void)frontend::askDaemon(socket, mtd::Shutdown{1}, e);
    daemon.stop();
    server.join();
    std::filesystem::remove_all(dir);
    return 0;
}
