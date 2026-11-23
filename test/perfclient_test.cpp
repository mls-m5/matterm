#include "mls-unit-test/unittest.h"
#include "perfclient.h"
#include <fstream>

using namespace frontend;
TEST_SUIT_BEGIN(PerfClient)

TEST_CASE("Echo latency runs from the first key of a burst to first output") {
    perf::EchoMeter m;
    m.outputArrived(5);
    EXPECT(m.samples().empty());
    m.keySent(100);
    m.keySent(104);
    m.outputArrived(130);
    m.outputArrived(135);
    EXPECT_EQ(m.samples().summary().count, std::size_t(1));
    EXPECT_EQ(m.samples().summary().avg, 30.0);
}

TEST_CASE("Host write backpressure includes the wait after EAGAIN") {
    HostWriteWait wait;
    EXPECT_EQ(wait.take(10), 0.0);
    wait.blocked(20);
    wait.blocked(40); // Repeated EAGAIN does not restart the wait.
    EXPECT_EQ(wait.take(75), 55.0);
    EXPECT_EQ(wait.take(80), 0.0);
}

TEST_CASE("A client run shorter than one second writes a partial interval and "
          "summary") {
    char path[] = "/tmp/matterm-clientperf-XXXXXX";
    const int fd = ::mkstemp(path);
    EXPECT(fd >= 0);
    ::close(fd);
    {
        ClientPerf meter(path, "client-test");
        EXPECT(meter.enabled());
        meter.second.counters.layoutMs = 2.5;
        meter.second.echo.add(3);
    }
    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    ::unlink(path);
    EXPECT(text.find("interval_s=") != std::string::npos);
    EXPECT(text.find("layout_ms=2.5") != std::string::npos);
    EXPECT(text.find("summary") != std::string::npos);
    EXPECT(text.find("verdict") != std::string::npos);
}
TEST_SUIT_END
