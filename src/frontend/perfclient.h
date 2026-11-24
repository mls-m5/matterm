#pragma once

// Client lifecycle around the shared diagnostics writer. No clock is read when
// disabled; the final partial interval is submitted before the shared summary.
#include "../perflog.h"
#include <chrono>

namespace frontend {
namespace perf = ::perf;

inline double perfClockMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class ClientPerf {
public:
    ClientPerf(const std::string &path, const std::string &role)
        : writer_(path, role) {
        if (enabled()) {
            at_ = perfClockMs();
            cpu_ = perf::processCpuMs();
        }
    }
    ~ClientPerf() {
        flush();
    }
    bool enabled() const {
        return writer_.enabled();
    }
    const std::string &error() const {
        return writer_.error();
    }
    void phase(std::string_view name,
               double elapsedMs,
               bool success = true,
               bool begin = false) {
        writer_.phase(name, elapsedMs, success, begin);
    }
    void tick() {
        if (enabled() && perfClockMs() - at_ >= 1000) {
            flush();
        }
    }
    void flush() {
        if (!enabled())
            return;
        const double now = perfClockMs(), cpu = perf::processCpuMs();
        if (now > at_) {
            second.cpuMs = cpu - cpu_;
            writer_.add(second, (now - at_) / 1000.0);
            second = {};
            at_ = now;
            cpu_ = cpu;
        }
    }
    perf::Second second;

private:
    perf::Writer writer_;
    double at_ = 0, cpu_ = 0;
};

// Time for which a nonblocking host write was waiting for capacity. The caller
// supplies the clock only while diagnostics are enabled. Successful write call
// duration is measured separately; EAGAIN includes the subsequent poll wait.
class HostWriteWait {
public:
    void blocked(double now) {
        if (!waiting_) {
            waiting_ = true;
            at_ = now;
        }
    }
    double take(double now) {
        if (!waiting_)
            return 0;
        const double elapsed = std::max(0.0, now - at_);
        waiting_ = false;
        return elapsed;
    }

private:
    bool waiting_ = false;
    double at_ = 0;
};
} // namespace frontend
