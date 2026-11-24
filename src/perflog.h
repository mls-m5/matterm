#pragma once

// Opt-in performance diagnostics, shared by daemon and clients. No terminal
// content is accepted here. Callers gate ALL timing/sampling on enabled().
// Samples use bounded logarithmic histograms: zero, <=0.125 ms, eight buckets
// per octave through 65536 ms, then overflow. Count/sum/max are exact; p95 is
// the selected bucket's upper bound (overflow uses observed max). Thus p95
// is approximate, with <9.1% relative bucket width above the lowest bucket.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>

namespace perf {
struct Summary {
    std::size_t count = 0;
    double avg = 0, p95 = 0, max = 0;
};
class Samples {
public:
    void add(double value) {
        if (!std::isfinite(value) || value < 0)
            return;
        std::size_t bucket = 0;
        if (value > 0) {
            std::size_t low = 1, high = 154;
            while (low < high) {
                const auto mid = low + (high - low) / 2;
                if (value <= upper(mid))
                    high = mid;
                else
                    low = mid + 1;
            }
            bucket = low;
        }
        ++bins_[bucket];
        ++count_;
        sum_ += value;
        max_ = std::max(max_, value);
    }
    bool empty() const {
        return count_ == 0;
    }
    void clear() {
        *this = Samples{};
    }
    void merge(const Samples &other) {
        for (std::size_t i = 0; i < bins_.size(); ++i)
            bins_[i] += other.bins_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        max_ = std::max(max_, other.max_);
    }
    Summary summary() const {
        if (empty())
            return {};
        const auto rank =
            count_ - count_ / 20; // ceil(0.95 * count), no rounding loss
        std::size_t cumulative = 0;
        double p95 = 0;
        for (std::size_t i = 0; i < bins_.size(); ++i) {
            cumulative += bins_[i];
            if (cumulative >= rank) {
                p95 = i == 154 ? max_ : upper(i);
                break;
            }
        }
        return {count_, sum_ / double(count_), p95, max_};
    }
    Summary take() {
        const auto s = summary();
        clear();
        return s;
    }

private:
    static double upper(std::size_t bucket) {
        return bucket == 0 ? 0 : std::exp2(double(bucket - 1) / 8.0 - 3.0);
    }
    std::array<std::size_t, 155> bins_{};
    std::size_t count_ = 0;
    double sum_ = 0, max_ = 0;
};
class EchoMeter {
public:
    void keySent(double nowMs) {
        if (!waiting_) {
            waiting_ = true;
            sentAt_ = nowMs;
        }
    }
    void outputArrived(double nowMs) {
        if (waiting_) {
            waiting_ = false;
            samples_.add(std::max(0.0, nowMs - sentAt_));
        }
    }
    Samples &samples() {
        return samples_;
    }

private:
    bool waiting_ = false;
    double sentAt_ = 0;
    Samples samples_;
};
struct Counters {
    std::uint64_t bytesIn = 0, bytesOut = 0, messagesIn = 0, messagesOut = 0;
    std::uint64_t largestIn = 0, largestOut = 0, stalls = 0, ptyBytes = 0,
                  largestPtyRound = 0;
    double parseMs = 0, layoutMs = 0, drawMs = 0, blockedMs = 0,
           backpressureMs = 0, windowBlockedSessionMs = 0;
    void in(std::size_t bytes) {
        bytesIn += bytes;
        ++messagesIn;
        largestIn = std::max<std::uint64_t>(largestIn, bytes);
    }
    void out(std::size_t bytes) {
        bytesOut += bytes;
        ++messagesOut;
        largestOut = std::max<std::uint64_t>(largestOut, bytes);
    }
};
struct Second {
    Counters counters;
    Samples echo, rtt, pump;
    Samples guiLoopGap, guiPumpRound, guiPumpFocused, guiPumpVisible,
        guiPumpHidden;
    double cpuMs = 0;
    std::uint64_t sessions = 0, clients = 0;
};
class Totals {
public:
    void add(const Second &s, double elapsedSeconds) {
        if (!(elapsedSeconds > 0) || !std::isfinite(elapsedSeconds))
            return;
        elapsed_ += elapsedSeconds;
        ++intervals_;
        auto &a = total_.counters;
        const auto &b = s.counters;
        a.bytesIn += b.bytesIn;
        a.bytesOut += b.bytesOut;
        a.messagesIn += b.messagesIn;
        a.messagesOut += b.messagesOut;
        a.largestIn = std::max(a.largestIn, b.largestIn);
        a.largestOut = std::max(a.largestOut, b.largestOut);
        a.stalls += b.stalls;
        a.ptyBytes += b.ptyBytes;
        a.largestPtyRound = std::max(a.largestPtyRound, b.largestPtyRound);
        a.parseMs += b.parseMs;
        a.layoutMs += b.layoutMs;
        a.drawMs += b.drawMs;
        a.blockedMs += b.blockedMs;
        a.backpressureMs += b.backpressureMs;
        a.windowBlockedSessionMs += b.windowBlockedSessionMs;
        total_.echo.merge(s.echo);
        total_.rtt.merge(s.rtt);
        total_.pump.merge(s.pump);
        total_.guiLoopGap.merge(s.guiLoopGap);
        total_.guiPumpRound.merge(s.guiPumpRound);
        total_.guiPumpFocused.merge(s.guiPumpFocused);
        total_.guiPumpVisible.merge(s.guiPumpVisible);
        total_.guiPumpHidden.merge(s.guiPumpHidden);
        total_.cpuMs += s.cpuMs;
        total_.sessions = s.sessions;
        total_.clients = s.clients;
    }
    const Second &total() const {
        return total_;
    }
    double elapsedSeconds() const {
        return elapsed_;
    }
    std::size_t seconds() const {
        return intervals_;
    }

private:
    Second total_;
    double elapsed_ = 0;
    std::size_t intervals_ = 0;
};
inline std::string number(double value, int decimals = 1) {
    // Enough for any finite double in fixed notation, including sign, decimal
    // point and six fractional digits. Bound precision to keep logging cheap.
    char buffer[std::numeric_limits<double>::max_exponent10 + 32];
    const int precision = decimals < 0 ? 0 : decimals > 6 ? 6 : decimals;
    (void)std::snprintf(buffer, sizeof buffer, "%.*f", precision, value);
    return buffer;
}
inline std::string formatSamples(const char *name, const Samples &samples) {
    const auto s = samples.summary();
    return std::string(name) + "_n=" + std::to_string(s.count) +
           " avg_ms=" + number(s.avg) + " p95_approx_ms=" + number(s.p95) +
           " max_ms=" + number(s.max);
}
inline std::string formatSecond(const std::string &who,
                                double elapsedSeconds,
                                const Second &s,
                                double intervalSeconds) {
    const auto &c = s.counters;
    return who + " t=" + number(elapsedSeconds, 3) +
           " interval_s=" + number(intervalSeconds, 3) + " " +
           formatSamples("echo", s.echo) + " | " + formatSamples("rtt", s.rtt) +
           " | " + formatSamples("pump", s.pump) + " | " +
           formatSamples("gui_loop_gap", s.guiLoopGap) + " | " +
           formatSamples("gui_pump_round", s.guiPumpRound) + " | " +
           formatSamples("gui_pump_focused", s.guiPumpFocused) + " | " +
           formatSamples("gui_pump_visible", s.guiPumpVisible) + " | " +
           formatSamples("gui_pump_hidden", s.guiPumpHidden) +
           " | in_bytes=" + std::to_string(c.bytesIn) +
           " in_msg=" + std::to_string(c.messagesIn) +
           " largest_in=" + std::to_string(c.largestIn) +
           " out_bytes=" + std::to_string(c.bytesOut) +
           " out_msg=" + std::to_string(c.messagesOut) +
           " largest_out=" + std::to_string(c.largestOut) +
           " pty_bytes=" + std::to_string(c.ptyBytes) +
           " pty_bytes_per_pump_avg=" +
           number(s.pump.empty()
                      ? 0
                      : double(c.ptyBytes) / double(s.pump.summary().count)) +
           " pty_bytes_per_pump_max=" + std::to_string(c.largestPtyRound) +
           " parse_ms=" + number(c.parseMs) +
           " layout_ms=" + number(c.layoutMs) + " draw_ms=" + number(c.drawMs) +
           " blocked_ms=" + number(c.blockedMs) +
           " socket_write_pending_ms=" + number(c.backpressureMs) +
           " window_blocked_session_ms_sampled=" +
           number(c.windowBlockedSessionMs) +
           " stalls=" + std::to_string(c.stalls) +
           " sessions=" + std::to_string(s.sessions) +
           " clients=" + std::to_string(s.clients) +
           " cpu_ms=" + number(s.cpuMs) + " cpu=" +
           number(intervalSeconds > 0 ? s.cpuMs / (intervalSeconds * 10) : 0);
}
// Only local measured work is evidence. RTT alone cannot separate network,
// daemon scheduling and application delay. No automatic network diagnosis.
struct Numbers {
    double elapsedMs = 0, parseMs = 0, layoutMs = 0, drawMs = 0, blockedMs = 0,
           pumpMs = 0;
};
inline std::string verdict(const Numbers &n) {
    if (!(n.elapsedMs > 0))
        return "unknown";
    const double client = n.parseMs + n.layoutMs + n.drawMs;
    if (n.blockedMs > n.elapsedMs * 0.5)
        return "host terminal";
    if (client > n.elapsedMs * 0.5)
        return "client";
    if (n.pumpMs > n.elapsedMs * 0.5)
        return "daemon";
    return "unknown";
}
inline double processCpuMs() {
    rusage r{};
    if (::getrusage(RUSAGE_SELF, &r) != 0)
        return 0;
    return double(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000 +
           double(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000;
}
class Writer {
public:
    Writer(std::string path, std::string who)
        : who_(std::move(who) + " pid=" + std::to_string(::getpid())) {
        if (path.empty()) {
            const char *env = std::getenv("MATTERM_PERF_LOG");
            if (env)
                path = env;
        }
        if (path.empty())
            return;
        fd_ = ::open(path.c_str(),
                     O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK,
                     0600);
        if (fd_ < 0)
            error_ = std::strerror(errno);
    }
    ~Writer() {
        finish();
        if (fd_ >= 0)
            ::close(fd_);
    }
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    bool enabled() const {
        return fd_ >= 0 && error_.empty() && !finished_;
    }
    const std::string &error() const {
        return error_;
    }
    void add(const Second &s, double intervalSeconds) {
        if (!enabled() || !(intervalSeconds > 0) ||
            !std::isfinite(intervalSeconds))
            return;
        totals_.add(s, intervalSeconds);
        line(formatSecond(who_, totals_.elapsedSeconds(), s, intervalSeconds));
    }
    void finish() {
        if (!enabled())
            return;
        const auto &s = totals_.total();
        const auto &c = s.counters;
        line(formatSecond(who_ + " summary",
                          totals_.elapsedSeconds(),
                          s,
                          totals_.elapsedSeconds()) +
             " verdict=" +
             verdict({totals_.elapsedSeconds() * 1000,
                      c.parseMs,
                      c.layoutMs,
                      c.drawMs,
                      c.blockedMs,
                      s.pump.summary().avg * double(s.pump.summary().count)}) +
             " hint_only=1");
        finished_ = true;
    }

private:
    void line(std::string text) {
        text += '\n';
        ssize_t n;
        do {
            n = ::write(fd_, text.data(), text.size());
        } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(text.size()))
            error_ =
                n < 0 ? std::strerror(errno) : "short performance log write";
    }
    std::string who_, error_;
    int fd_ = -1;
    bool finished_ = false;
    Totals totals_;
};
} // namespace perf
