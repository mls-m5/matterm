#include "bigfont.h"
#include "color.h"
#include "programs.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// Timing, in named constants.
constexpr Time kHold{900};        // The word stands intact.
constexpr Time kShake{700};       // It trembles.
constexpr int kStepMs = 33;       // Fixed simulation step.
constexpr double kGravity = 70.0; // Rows per second squared.
constexpr double kBounce = 0.42;  // Speed kept after a bounce.
constexpr double kRest = 5.0;     // Below this speed a block stays down.
constexpr Time kLimit{25000};     // Ends by itself after this long.

// A word in big letters that cracks into blocks, which fall, bounce and pile
// up on the floor. Space plays it again.
class Crumble final : public Program {
public:
    explicit Crumble(const std::vector<std::string> &args) {
        for (const auto &arg : args) {
            word_ += (word_.empty() ? "" : " ") + arg;
        }
        if (word_.empty()) {
            word_ = "MATTERM";
        }
    }

    void begin(World &world, Time now) override {
        start_ = now;
        stepped_ = now;
        (void)world;
    }

    void key(World &, const Key &key) override {
        if (key.isText("q") || key.isText("Q") || key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c")) {
            finished_ = true;
        }
        else if (key.isText(" ")) {
            blocks_.clear(); // Play again.
            restart_ = true;
        }
    }

    void resize(World &, int, int) override {
        blocks_.clear();
        restart_ = true;
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        if (blocks_.empty()) {
            build(world, w, h, now);
        }
        const Time elapsed = now - start_;
        while (stepped_ + Time(kStepMs) <= now) {
            step(world, w, h, stepped_ - start_);
            stepped_ += Time(kStepMs);
        }
        canvas.clear({8, 8, 14});
        const bool unicode = world.caps.unicode;
        for (const auto &b : blocks_) {
            int x = int(std::lround(b.x)), y = int(std::lround(b.y));
            if (!b.fallen && elapsed >= kHold && elapsed < kHold + kShake) {
                x += int(world.rng.below(3)) - 1; // Tremble.
            }
            const Rgb color =
                b.landed ? mix(b.color, {40, 40, 50}, 0.45) : b.color;
            canvas.put(x, y, unicode ? U'█' : U'#', color, {8, 8, 14});
        }
        for (int x = 0; x < w; ++x) {
            canvas.put(x, h - 2, U'=', {90, 90, 110}, {8, 8, 14});
        }
        canvas.text(1,
                    h - 1,
                    " crumble   q quit   space: again ",
                    {230, 230, 230},
                    {20, 20, 20});
        if (elapsed >= kLimit) {
            finished_ = true;
        }
    }

    bool finished() const override {
        return finished_;
    }
    void end(World &world) override {
        world.flags.insert("crumbled");
    }

private:
    struct Block {
        double x, y, vx = 0, vy = 0;
        Time fallAt{0};
        bool falling = false, landed = false, fallen = false;
        Rgb color;
    };

    void build(World &world, int w, int h, Time now) {
        // Whole letters only: drop the ones that do not fit.
        std::string text = word_;
        while (text.size() > 1 && int(text.size()) * 6 - 1 > w - 2) {
            text.pop_back();
        }
        const int width = int(text.size()) * 6 - 1;
        const int left = std::max(0, (w - width) / 2);
        const int top = std::max(0, (h - 2) / 3 - 2);
        blocks_.clear();
        for (const auto &[cx, cy] : bigText(text)) {
            Block b;
            b.x = left + cx;
            b.y = top + cy;
            // The crack runs from the top down, with some disorder.
            b.fallAt =
                kHold + kShake + Time(int(cy * 60 + world.rng.below(900)));
            b.color =
                hsv(0.55 + double(cx) / std::max(1, width) * 0.35, 0.6, 1.0);
            blocks_.push_back(b);
        }
        height_.assign(std::size_t(w), 0);
        if (restart_ || start_ == Time(0)) {
            start_ = now;
            stepped_ = now;
        }
        restart_ = false;
    }

    void step(World &world, int w, int h, Time elapsed) {
        const double dt = kStepMs / 1000.0;
        const int floorRow = h - 3; // Above the ground line.
        for (auto &b : blocks_) {
            if (b.landed) {
                continue;
            }
            if (!b.falling) {
                if (elapsed >= b.fallAt) {
                    b.falling = true;
                    b.fallen = true;
                    b.vx = (world.rng.unit() - 0.5) * 14.0;
                    b.vy = world.rng.unit() * 3.0;
                }
                continue;
            }
            b.vy += kGravity * dt;
            b.y += b.vy * dt;
            b.x = std::clamp(b.x + b.vx * dt, 0.0, double(w - 1));
            const int column = std::clamp(int(std::lround(b.x)), 0, w - 1);
            const double ground = floorRow - height_[std::size_t(column)];
            if (b.y >= ground) {
                b.y = ground;
                if (std::abs(b.vy) * kBounce < kRest) {
                    b.landed = true;
                    b.x = column;
                    ++height_[std::size_t(column)];
                }
                else {
                    b.vy = -b.vy * kBounce;
                    b.vx *= 0.6;
                }
            }
        }
    }

    std::string word_;
    std::vector<Block> blocks_;
    std::vector<int> height_;
    Time start_{0}, stepped_{0};
    bool finished_ = false;
    bool restart_ = false;
};

} // namespace

std::unique_ptr<Program> makeCrumble(const std::vector<std::string> &args) {
    return std::make_unique<Crumble>(args);
}

} // namespace fakeos
