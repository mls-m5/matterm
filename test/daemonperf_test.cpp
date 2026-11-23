#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"
#include <fstream>

TEST_SUIT_BEGIN(DaemonPerf)

TEST_CASE("Real daemon logs editor-like PTY output and keeps existing Ping "
          "compatible") {
    const std::string log =
        "/tmp/matterm-daemon-perf-" + std::to_string(getpid());
    ::unlink(log.c_str());
    Running r(60, std::nullopt, {}, [&](mtd::DaemonConfig &c) {
        c.perfLogPath = log;
    });
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(attach(c).has_value());
    EXPECT(c.send(protocol::Ping{123456}));
    bool pong = false;
    for (int i = 0; i < 20 && !pong; ++i) {
        const auto m = c.server();
        if (!m)
            break;
        if (const auto *p = std::get_if<protocol::Pong>(&*m))
            pong = p->nonce == 123456;
    }
    EXPECT(pong);
    EXPECT(c.send(protocol::Input{"printf '\\033[2J\\033[H\\033[32m'; seq 1 "
                                  "1000; sleep 1.2; echo PERF_DONE\n"}));
    EXPECT(c.outputContains("\r\nPERF_DONE\r\n"));
    r.stop();
    std::ifstream in(log);
    std::string line;
    int lines = 0, summaries = 0;
    bool bytes = false, pumps = false;
    while (std::getline(in, line)) {
        ++lines;
        if (line.find(" summary ") != std::string::npos)
            ++summaries;
        EXPECT(line.find("pid=") != std::string::npos);
        EXPECT(line.find("PERF_DONE") == std::string::npos);
        if (line.find("pty_bytes=0 ") == std::string::npos)
            bytes = true;
        if (line.find("pump_n=0 ") == std::string::npos)
            pumps = true;
    }
    EXPECT(lines >= 2);
    EXPECT(summaries == 1);
    EXPECT(bytes && pumps);
    ::unlink(log.c_str());
}

TEST_SUIT_END
