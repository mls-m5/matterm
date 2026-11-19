#include "programs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fakeos {
namespace {

std::string pad(const std::string &s, std::size_t width, bool right = true) {
    if (s.size() >= width) {
        return s.substr(0, width);
    }
    return right ? std::string(width - s.size(), ' ') + s
                 : s + std::string(width - s.size(), ' ');
}

struct Row {
    int pid;
    const char *user;
    const char *command;
    double cpu;
    double mem;
};

// A pretend `top`: the numbers drift once a second.
class Top final : public Program {
public:
    void begin(World &, Time now) override {
        updated_ = now - Time(1000);
        rows_ = {{1, "root", "init", 0.0, 0.1},
                 {42, "guest", "matterm-sh", 0.3, 0.4},
                 {57, "root", "cron", 0.0, 0.1},
                 {63, "root", "syslogd", 0.1, 0.2},
                 {88, "guest", "top", 0.7, 0.3},
                 {101, "root", "kworker/0:1", 0.0, 0.0},
                 {137, "guest", "plasma-idle", 0.2, 1.1},
                 {212, "root", "rabbit-holed", 0.0, 0.5}};
    }
    void key(World &, const Key &key) override {
        if (key.isText("q") || key.isText("Q") || key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c")) {
            finished_ = true;
        }
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        if (now - updated_ >= Time(1000)) {
            updated_ = now;
            for (auto &row : rows_) {
                if (row.pid != 1) {
                    row.cpu =
                        std::max(0.0, row.cpu + (world.rng.unit() - 0.5) * 1.2);
                    row.cpu = std::min(row.cpu, 12.0);
                }
            }
            std::stable_sort(
                rows_.begin(), rows_.end(), [](const Row &a, const Row &b) {
                    return a.cpu > b.cpu;
                });
        }
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear();
        const Rgb text{220, 220, 220}, head{0, 0, 0}, back{0, 0, 0},
            accent{120, 200, 255};
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(world.now)
                .count() +
            3 * 86400 + 4 * 3600 + 1020;
        canvas.text(0,
                    0,
                    "top - up " + std::to_string(uptime / 86400) +
                        " days, 1 user, load average: 0.00, 0.01, 0.00",
                    accent,
                    back,
                    true);
        canvas.text(0,
                    1,
                    "Tasks: " + std::to_string(rows_.size()) +
                        " total, 1 running, " +
                        std::to_string(rows_.size() - 1) + " sleeping",
                    text,
                    back);
        canvas.text(0,
                    2,
                    "MiB Mem : 1024.0 total,  588.0 free,  209.0 used,  227.0 "
                    "buff/cache",
                    text,
                    back);
        canvas.text(0,
                    4,
                    pad("PID", 5) + " " + pad("USER", 8, false) + " " +
                        pad("%CPU", 5) + " " + pad("%MEM", 5) + " COMMAND",
                    head,
                    {200, 200, 200});
        for (std::size_t i = 0; i < rows_.size() && int(i) + 5 < h - 1; ++i) {
            const auto &r = rows_[i];
            auto num = [](double v) {
                const int tenths = int(v * 10 + 0.5);
                return std::to_string(tenths / 10) + "." +
                       std::to_string(tenths % 10);
            };
            canvas.text(0,
                        int(i) + 5,
                        pad(std::to_string(r.pid), 5) + " " +
                            pad(r.user, 8, false) + " " + pad(num(r.cpu), 5) +
                            " " + pad(num(r.mem), 5) + " " + r.command,
                        text,
                        back);
        }
        canvas.text(0, h - 1, "q quit", {130, 130, 150}, back);
        (void)w;
    }
    bool finished() const override {
        return finished_;
    }
    Time interval() const override {
        return Time(100);
    }

private:
    std::vector<Row> rows_;
    Time updated_{0};
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeTop(const std::vector<std::string> &) {
    return std::make_unique<Top>();
}

} // namespace fakeos
