#include "color.h"
#include "programs.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// A small aquarium: fish of a few kinds swim back and forth, bubbles rise and
// the weed sways. Space feeds the fish (a few flakes fall and the fish hurry).
class Aquarium final : public Program {
public:
    void begin(World &world, Time now) override {
        last_ = now;
        for (int i = 0; i < 7; ++i) {
            fish_.push_back({world.rng.unit() * 60, 2 + world.rng.unit() * 14,
                             (world.rng.below(2) ? 1.0 : -1.0) * (3 + world.rng.unit() * 5),
                             int(world.rng.below(3)), world.rng.unit() * 6.28});
        }
    }
    void key(World &world, const Key &key) override {
        if (key.is(Key::Type::Escape) || (key.is(Key::Type::Ctrl) && key.text == "c") ||
            key.isText("q") || key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText(" ")) {
            for (int i = 0; i < 6; ++i) {
                flakes_.push_back({world.rng.unit() * 40 + 5, 0.0});
            }
        }
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        const double dt = std::min(0.1, double((now - last_).count()) / 1000.0);
        last_ = now;
        t_ += dt;
        for (int y = 0; y < h; ++y) {
            const Rgb water = mix({8, 40, 70}, {4, 18, 40}, double(y) / std::max(1, h - 1));
            for (int x = 0; x < w; ++x) {
                canvas.put(x, y, U' ', water, water);
            }
        }
        // Weed on the floor.
        for (int x = 4; x < w; x += 11) {
            for (int k = 0; k < 4; ++k) {
                const int sway = int(std::lround(std::sin(t_ * 1.3 + x + k * 0.7)));
                canvas.put(x + sway, h - 2 - k, k % 2 ? U')' : U'(', {40, 150, 70}, {4, 18, 40});
            }
        }
        canvas.text(0, h - 1, std::string(std::size_t(w), '~'), {160, 140, 90}, {40, 30, 20});
        // Bubbles.
        if (world.rng.below(8) == 0 && !fish_.empty()) {
            bubbles_.push_back({double(world.rng.below(std::uint32_t(std::max(1, w)))), double(h - 3)});
        }
        for (auto &b : bubbles_) {
            b.y -= dt * 4;
            canvas.put(int(b.x), int(b.y), U'o', {150, 200, 230}, {6, 28, 55});
        }
        bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [](const Dot &b) { return b.y < 0; }), bubbles_.end());
        // Flakes sink; fish near them speed up.
        for (auto &f : flakes_) {
            f.y += dt * 3;
            canvas.put(int(f.x), int(f.y), U'.', {230, 200, 120}, {6, 28, 55});
        }
        flakes_.erase(std::remove_if(flakes_.begin(), flakes_.end(), [&](const Dot &f) { return f.y >= h - 2; }), flakes_.end());
        // Fish.
        static const char *right[] = {"><>", "><((o>", "><(((*>"};
        static const char *left[] = {"<><", "<o))><", "<*)))><"};
        for (auto &f : fish_) {
            const double hurry = flakes_.empty() ? 1.0 : 2.2;
            f.x += f.vx * dt * hurry;
            const std::string body = f.vx > 0 ? right[f.kind] : left[f.kind];
            if (f.x > w + 2) {
                f.x = -8;
            }
            if (f.x < -9) {
                f.x = w + 1;
            }
            const double wobble = std::sin(t_ * 1.7 + f.phase) * 1.2;
            const Rgb fur = hsv(f.kind * 0.18 + 0.04, 0.7, 1.0);
            canvas.text(int(f.x), std::clamp(int(f.y + wobble), 0, std::max(0, h - 3)), body, fur, {6, 28, 55}, true);
        }
        canvas.text(1, 0, " aquarium   q quit   space feed ", {210, 225, 235}, {10, 40, 80});
    }
    bool finished() const override {
        return finished_;
    }
    std::string farewell() const override {
        return "The fish have already forgotten you.\n";
    }
    Time interval() const override {
        return Time(50);
    }

private:
    struct Fish {
        double x, y, vx;
        int kind;
        double phase;
    };
    struct Dot {
        double x, y;
    };
    std::vector<Fish> fish_;
    std::vector<Dot> bubbles_, flakes_;
    Time last_{0};
    double t_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeAquarium(const std::vector<std::string> &) {
    return std::make_unique<Aquarium>();
}

} // namespace fakeos
