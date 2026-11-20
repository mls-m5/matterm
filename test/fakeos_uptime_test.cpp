#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"

#include <string>

// Mattrix OS has been up since 1987-02-27 (a birthday): every place that shows
// the uptime or the boot time tells the same story, counted from the clock the
// host provides (Config::unixTime) and the session's own time.
using namespace fakeos;
using fakeos_test::Pc;

namespace {
constexpr long long kBoot = 541382400; // 1987-02-27 00:00:00 UTC.

Config at(long long unixTime) {
    Config config;
    config.intro = false;
    config.unixTime = unixTime;
    config.columns = 100;
    config.rows = 40;
    return config;
}
} // namespace

TEST_SUIT_BEGIN(FakeosUptime)

TEST_CASE("uptime counts days since 1987-02-27 from the host's date") {
    // The default date, 2026-01-01, is 14188 days later.
    Pc pc(at(1767225600));
    pc.run("uptime");
    EXPECT(pc.shows("up 14188 days,  0:00"));
    pc.run("clear");
    // The user's own estimate: 2026-11-19 is 14510 days.
    Pc november(at(1795046400));
    november.run("uptime");
    EXPECT(november.shows("up 14510 days,  0:00"));
}

TEST_CASE("The session's own time keeps adding to it") {
    Pc pc(at(1767225600));
    for (int i = 0; i < 4000; ++i) { // About two minutes of host time.
        pc.advance(33);
    }
    pc.run("uptime");
    EXPECT(pc.shows("up 14188 days,  0:02"));
}

TEST_CASE("uptime -s and -p") {
    Pc pc(at(1767225600));
    pc.run("uptime -s");
    EXPECT(pc.shows("1987-02-27 00:00:00"));
    pc.run("uptime -p");
    EXPECT(pc.shows("up 2026 weeks, 6 days, 0 hours, 0 minutes"));
}

TEST_CASE("neofetch shows the span in years, months and days") {
    Pc pc(at(1767225600));
    pc.run("neofetch");
    EXPECT(pc.shows("Uptime: 38 years, 10 months, 5 days"));
    Pc november(at(1795046400)); // 2026-11-19.
    november.run("neofetch");
    EXPECT(november.shows("Uptime: 39 years, 8 months, 23 days"));
}

TEST_CASE("top agrees") {
    Pc pc(at(1767225600));
    pc.run("top");
    pc.advance(100);
    EXPECT(pc.shows("up 14188 days"));
    pc.type("q");
}

TEST_CASE("/proc/uptime agrees and moves") {
    Pc pc(at(1767225600));
    pc.run("cat /proc/uptime");
    EXPECT(pc.shows("1225843200."));
    for (int i = 0; i < 100; ++i) {
        pc.advance(33);
    }
    pc.run("clear");
    pc.run("cat /proc/uptime");
    EXPECT(pc.shows("1225843204.") || pc.shows("1225843203."));
}

TEST_CASE("who -b and last reboot name the boot time") {
    Pc pc(at(1767225600));
    pc.run("who -b");
    EXPECT(pc.shows("system boot  1987-02-27 00:00"));
    pc.run("clear");
    pc.run("last reboot");
    EXPECT(pc.shows("reboot   system boot  6.8.0-mattrix"));
    EXPECT(pc.shows("Fri Feb 27 00:00"));
    EXPECT(pc.shows("still running"));
    EXPECT(pc.shows("wtmp begins"));
    pc.run("clear");
    pc.run("who");
    EXPECT(pc.shows("guest"));
    EXPECT(pc.shows("pts/0"));
    EXPECT(pc.shows("2026-01-01"));
}

TEST_CASE("The uptime follows a different host date, deterministically") {
    Pc a(at(1000000000)); // 2001-09-09.
    a.run("uptime");
    EXPECT(a.shows("up 5308 days"));
    Pc b(at(1000000000));
    b.run("uptime");
    EXPECT(a.screen() == b.screen());
}

TEST_SUIT_END
