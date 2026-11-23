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
