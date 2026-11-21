#include "color.h"
#include "programs.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fakeos {
namespace {

// Stars rush past as if the machine were very fast. + and - change the speed.
class Starfield final : public Program {
public:
    void begin(World &, Time now) override {
        last_ = now;
    }
    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Escape) || (key.is(Key::Type::Ctrl) && key.text == "c") ||
            key.isText("q") || key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText("+") || key.isText("=")) {
            speed_ = std::min(8.0, speed_ * 1.4);
        }
        else if (key.isText("-")) {
            speed_ = std::max(0.2, speed_ / 1.4);
        }
    }
    void resize(World &, int, int) override {
        stars_.clear();
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        if (stars_.empty()) {
            for (int i = 0; i < std::max(30, w * h / 12); ++i) {
                stars_.push_back(spawn(world, true));
            }
        }
        const double dt = std::min(0.1, double((now - last_).count()) / 1000.0);
        last_ = now;
        canvas.clear({0, 0, 6});
        for (auto &s : stars_) {
            s.z -= dt * speed_ * 0.8;
            if (s.z <= 0.04) {
                s = spawn(world, false);
            }
            // Perspective; cells are about twice as tall as wide.
            const double px = s.x / s.z * (w / 2.0) * 0.5 + w / 2.0;
            const double py = s.y / s.z * (h / 2.0) * 0.5 + h / 2.0;
            const int ix = int(std::lround(px)), iy = int(std::lround(py));
            if (ix < 0 || ix >= w || iy < 0 || iy >= h) {
                s = spawn(world, false);
                continue;
            }
            const double near = 1.0 - std::min(1.0, s.z);
            const char32_t glyph = near > 0.8 ? U'@' : near > 0.55 ? U'*' : near > 0.3 ? U'+' : U'.';
            const int v = 90 + int(165 * near);
            canvas.put(ix, iy, glyph, {std::uint8_t(v), std::uint8_t(v), std::uint8_t(std::min(255, v + 25))}, {0, 0, 6}, near > 0.6);
        }
        canvas.text(1, h - 1, " starfield   q quit   + / - speed ", {200, 200, 215}, {20, 20, 40});
    }
    bool finished() const override {
        return finished_;
    }
    std::string farewell() const override {
        return "Warp drive disengaged.\n";
    }
    Time interval() const override {
        return Time(40);
    }

private:
    struct Star {
        double x, y, z;
    };
    static Star spawn(World &world, bool anywhere) {
        return {world.rng.unit() * 2 - 1,
                world.rng.unit() * 2 - 1,
                anywhere ? 0.05 + world.rng.unit() : 1.0};
    }
    std::vector<Star> stars_;
    Time last_{0};
    double speed_ = 1.0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeStarfield(const std::vector<std::string> &) {
    return std::make_unique<Starfield>();
}

} // namespace fakeos
