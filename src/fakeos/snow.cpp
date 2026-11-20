#include "color.h"
#include "programs.h"

#include <algorithm>
#include <vector>

namespace fakeos {
namespace {

// Quiet snow: flakes drift down on a night sky and pile up on the ground.
// Left and right arrows (or a / d) set the wind.
class Snow final : public Program {
public:
    void begin(World &, Time now) override {
        stepped_ = now;
    }

    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c") || key.isText("q") ||
            key.isText("Q")) {
            finished_ = true;
        }
        else if (key.is(Key::Type::Left) || key.isText("a")) {
            wind_ = std::max(-2, wind_ - 1);
        }
        else if (key.is(Key::Type::Right) || key.isText("d")) {
            wind_ = std::min(2, wind_ + 1);
        }
    }

    void resize(World &, int, int) override {
        flakes_.clear();
        pile_.clear();
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        if (pile_.size() != std::size_t(w)) {
            pile_.assign(std::size_t(w), 0);
            flakes_.clear();
            // Start with a sky that is already snowing.
            for (int i = 0; i < w * h / 25; ++i) {
                flakes_.push_back(spawn(world, w, h, true));
            }
            stepped_ = now;
        }
        int steps = 0;
        while (stepped_ + Time(90) <= now && steps < 4) {
            step(world, w, h);
            stepped_ += Time(90);
            ++steps;
        }
        if (stepped_ + Time(90) <= now) {
            stepped_ = now;
        }
        draw(w, h, canvas);
    }

    bool finished() const override {
        return finished_;
    }

    std::string farewell() const override {
        return "The snow melts as soon as you look away.\n";
    }

private:
    struct Flake {
        int x, y;
        int size; // 0 small, 1 medium, 2 big (falls faster).
    };

    Flake spawn(World &world, int w, int h, bool anywhere) {
        const int size = int(world.rng.below(3));
        return {int(world.rng.below(std::uint32_t(w))),
                anywhere ? int(world.rng.below(std::uint32_t(h))) : 0,
                size};
    }

    void step(World &world, int w, int h) {
        for (int i = 0; i < std::max(1, w / 14); ++i) {
            flakes_.push_back(spawn(world, w, h, false));
        }
        for (auto it = flakes_.begin(); it != flakes_.end();) {
            it->y += 1 + (it->size == 2);
            const int drift = int(world.rng.below(3)) - 1 + wind_;
            if (drift != 0 && world.rng.below(2)) {
                it->x += drift > 0 ? 1 : -1;
            }
            it->x = (it->x + w) % w; // Wrap around the edges.
            const int ground = h - 1 - pile_[std::size_t(it->x)];
            if (it->y >= ground) {
                // Lands; low columns next to it fill first so the pile levels.
                int x = it->x;
                if (x > 0 &&
                    pile_[std::size_t(x - 1)] + 1 < pile_[std::size_t(x)]) {
                    --x;
                }
                else if (x + 1 < w && pile_[std::size_t(x) + 1] + 1 <
                                          pile_[std::size_t(x)]) {
                    ++x;
                }
                pile_[std::size_t(x)] =
                    std::min(pile_[std::size_t(x)] + 1, std::max(1, h / 3));
                it = flakes_.erase(it);
            }
            else {
                ++it;
            }
        }
        // A strong enough drift melts the tallest spots a little.
        if (world.rng.below(8) == 0) {
            const auto x = std::size_t(world.rng.below(std::uint32_t(w)));
            pile_[x] = std::max(0, pile_[x] - 1);
        }
    }

    void draw(int w, int h, AsciiCanvas &canvas) {
        for (int y = 0; y < h; ++y) {
            const double t = h > 1 ? double(y) / double(h - 1) : 0.0;
            const Rgb sky = mix({4, 8, 28}, {28, 40, 80}, t);
            for (int x = 0; x < w; ++x) {
                canvas.put(x, y, U' ', sky, sky);
            }
        }
        // A few fixed stars.
        for (int i = 0; i < w / 8; ++i) {
            const int x = (i * 37 + 11) % std::max(1, w);
            const int y = (i * 17 + 3) % std::max(1, h / 2);
            canvas.put(x, y, U'.', {130, 140, 170}, {4, 8, 28});
        }
        for (const auto &f : flakes_) {
            static const char32_t glyph[] = {U'.', U'*', U'*'};
            const double t = h > 1 ? double(f.y) / double(h - 1) : 0.0;
            canvas.put(f.x,
                       f.y,
                       glyph[f.size],
                       {235, 240, 255},
                       mix({4, 8, 28}, {28, 40, 80}, t),
                       f.size == 2);
        }
        for (int x = 0; x < w; ++x) {
            for (int k = 0; k < pile_[std::size_t(x)]; ++k) {
                const int y = h - 1 - k;
                canvas.put(x,
                           y,
                           k == pile_[std::size_t(x)] - 1 ? U'~' : U' ',
                           {255, 255, 255},
                           k == pile_[std::size_t(x)] - 1 ? Rgb{214, 224, 245}
                                                          : Rgb{236, 240, 252});
            }
        }
        canvas.text(1,
                    0,
                    " snow   q quit   left / right wind ",
                    {200, 205, 220},
                    {10, 14, 36});
    }

    std::vector<Flake> flakes_;
    std::vector<int> pile_;
    Time stepped_{0};
    int wind_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeSnow(const std::vector<std::string> &) {
    return std::make_unique<Snow>();
}

} // namespace fakeos
