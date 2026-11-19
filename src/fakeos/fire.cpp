#include "color.h"
#include "programs.h"

#include <algorithm>
#include <vector>

namespace fakeos {
namespace {

// The classic cellular fire: heat is injected along the bottom row, rises with
// a little random drift and cools on the way.
class Fire final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
        stepped_ = now;
    }

    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c") || key.isText("q") ||
            key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText("+") || key.isText("=")) {
            fuel_ = std::min(255, fuel_ + 25);
        }
        else if (key.isText("-")) {
            fuel_ = std::max(60, fuel_ - 25);
        }
    }

    void resize(World &, int, int) override {
        heat_.clear(); // Rebuilt at the next frame.
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        if (heat_.size() != std::size_t(w) * std::size_t(h)) {
            heat_.assign(std::size_t(w) * std::size_t(h), 0);
            stepped_ = now;
        }
        // At most a few simulation steps per frame, at a fixed rate.
        int steps = 0;
        while (stepped_ + Time(40) <= now && steps < 4) {
            step(world, w, h);
            stepped_ += Time(40);
            ++steps;
        }
        if (stepped_ + Time(40) <= now) {
            stepped_ = now;
        }
        elapsed_ = now - start_;
        draw(w, h, canvas);
    }

    bool finished() const override {
        return finished_;
    }

    void end(World &world) override {
        // Patience is rewarded: the fire leaves something in the ashes.
        if (elapsed_ >= Time(8000) && !world.flags.count("fire-watched")) {
            world.flags.insert("fire-watched");
            world.fs.put("/home/" + world.user + "/.ember",
                         "A single ember, still warm.\n"
                         "It says: the cave knows a word. Try saying it.\n",
                         world.user,
                         0644,
                         world.unixNow());
            reward_ = true;
        }
    }

    std::string farewell() const override {
        return reward_ ? "Something was left in the ashes.\n" : std::string();
    }

private:
    void step(World &world, int w, int h) {
        auto &rng = world.rng;
        for (int x = 0; x < w; ++x) { // Fuel row.
            heat_[std::size_t(h - 1) * std::size_t(w) + std::size_t(x)] =
                std::uint8_t(std::min<int>(255, fuel_ - int(rng.below(60))));
        }
        for (int y = 0; y < h - 1; ++y) {
            for (int x = 0; x < w; ++x) {
                const int drift = int(rng.below(3)) - 1;
                const int sx = std::clamp(x + drift, 0, w - 1);
                const int below = heat_[std::size_t(y + 1) * std::size_t(w) +
                                        std::size_t(sx)];
                const int cooled = below - int(rng.below(24)) -
                                   (h > 1 ? 255 / (h + 4) / 2 : 0);
                heat_[std::size_t(y) * std::size_t(w) + std::size_t(x)] =
                    std::uint8_t(std::max(0, cooled));
            }
        }
    }

    static Rgb palette(int heat) {
        const double t = heat / 255.0;
        if (t < 0.25)
            return mix({0, 0, 0}, {120, 10, 0}, t / 0.25);
        if (t < 0.5)
            return mix({120, 10, 0}, {255, 90, 0}, (t - 0.25) / 0.25);
        if (t < 0.75)
            return mix({255, 90, 0}, {255, 200, 40}, (t - 0.5) / 0.25);
        return mix({255, 200, 40}, {255, 255, 220}, (t - 0.75) / 0.25);
    }

    void draw(int w, int h, AsciiCanvas &canvas) {
        static const char ramp[] = " .:-=+*#%@";
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int v =
                    heat_[std::size_t(y) * std::size_t(w) + std::size_t(x)];
                const char glyph = ramp[std::min(9, v * 10 / 256)];
                canvas.put(x,
                           y,
                           char32_t(glyph),
                           palette(std::min(255, v + 40)),
                           mix({0, 0, 0}, palette(v), 0.55));
            }
        }
        canvas.text(1,
                    h - 1,
                    " fire   q quit   + / - fuel ",
                    {230, 230, 230},
                    {20, 20, 20});
    }

    std::vector<std::uint8_t> heat_;
    Time start_{0}, stepped_{0}, elapsed_{0};
    int fuel_ = 255;
    bool finished_ = false;
    bool reward_ = false;
};

} // namespace

std::unique_ptr<Program> makeFire(const std::vector<std::string> &) {
    return std::make_unique<Fire>();
}

} // namespace fakeos
