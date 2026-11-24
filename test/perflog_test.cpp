#include "mls-unit-test/unittest.h"
#include "perflog.h"
#include <fstream>
#include <limits>
#include <unistd.h>

TEST_SUIT_BEGIN(PerfLog)

TEST_CASE("Bounded samples merge counts and sums, not interval percentiles") {
    perf::Samples a, b;
    for (int i = 0; i < 100; ++i)
        a.add(1);
    b.add(1000);
    a.merge(b);
    const auto s = a.summary();
    EXPECT(s.count == 101u);
    EXPECT(s.avg == 1100.0 / 101.0);
    EXPECT(s.max == 1000);
    EXPECT(s.p95 >= 1 && s.p95 < 1.1);
    EXPECT(a.take().count == 101u);
    EXPECT(a.empty());
}
TEST_CASE("Histogram handles zero, overflow and invalid samples") {
    perf::Samples s;
    s.add(-1);
    s.add(std::numeric_limits<double>::infinity());
    EXPECT(s.empty());
    s.add(0);
    EXPECT(s.summary().p95 == 0);
    s.clear();
    s.add(100000);
    EXPECT(s.summary().p95 == 100000);
    s.clear();
    s.add(0.001);
    EXPECT(s.summary().p95 == 0.125);
    EXPECT(sizeof(perf::Samples) < 2048u);
}
TEST_CASE("Totals weight CPU by actual partial interval and merge samples") {
    perf::Second a, b;
    a.cpuMs = 50;
    b.cpuMs = 50;
    a.echo.add(1);
    b.echo.add(10);
    perf::Totals t;
    t.add(a, 1);
    t.add(b, 0.25);
    EXPECT(t.elapsedSeconds() == 1.25);
    EXPECT(t.total().cpuMs == 100);
    EXPECT(t.total().echo.summary().avg == 5.5);
    EXPECT(perf::formatSecond("test", 1.25, t.total(), 1.25).find("cpu=8.0") !=
           std::string::npos);
    EXPECT(perf::verdict({}) == "unknown");
}
TEST_CASE("GUI loop and per-pane pump histograms are bounded and merged without terminal data") {
    perf::Second a, b;
    a.guiLoopGap.add(40); b.guiLoopGap.add(80);
    a.guiPumpRound.add(30); b.guiPumpRound.add(60);
    a.guiPumpFocused.add(3); b.guiPumpFocused.add(6);
    a.guiPumpVisible.add(4); b.guiPumpVisible.add(8);
    a.guiPumpHidden.add(20); b.guiPumpHidden.add(40);
    perf::Totals totals;
    totals.add(a, 1); totals.add(b, 1);
    EXPECT(totals.total().guiLoopGap.summary().avg == 60);
    EXPECT(totals.total().guiPumpHidden.summary().count == 2);
    const auto line = perf::formatSecond("test", 2, totals.total(), 2);
    for (const auto *field : {"gui_loop_gap_n=2", "gui_pump_round_n=2", "gui_pump_focused_n=2", "gui_pump_visible_n=2", "gui_pump_hidden_n=2"})
        EXPECT(line.find(field) != std::string::npos);
    EXPECT(sizeof(perf::Second) < 20000u);
}

TEST_CASE("Numeric formatting bounds precision and does not truncate doubles") {
    EXPECT(perf::number(std::numeric_limits<double>::max()).size() > 300u);
    EXPECT_EQ(perf::number(1.25, 8), std::string("1.250000"));
    EXPECT_EQ(perf::number(1.25, -1), std::string("1"));
}
TEST_CASE("Writer flushes partial intervals and finishes exactly once") {
    const std::string path = "/tmp/matterm-perflog-" + std::to_string(getpid());
    ::unlink(path.c_str());
    {
        perf::Writer w(path, "test");
        EXPECT(w.enabled());
        perf::Second s;
        s.echo.add(2);
        w.add(s, 0.25);
        w.finish();
        w.finish();
    }
    std::ifstream in(path);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) {
        ++lines;
        EXPECT(line.find("pid=") != std::string::npos);
    }
    EXPECT(lines == 2);
    ::unlink(path.c_str());
}

TEST_SUIT_END
