#include "color.h"
#include "programs.h"

#include <algorithm>
#include <vector>

namespace fakeos {
namespace {

// Conway's Game of Life on a wrapping board, seeded from the world's random
// generator. Cells glow brighter when they are young. Space or r reseeds.
class Life final : public Program {
public:
    void begin(World &, Time now) override {
        stepped_ = now;
    }

    void key(World &world, const Key &key) override {
        if (key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c") || key.isText("q") ||
            key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText(" ") || key.isText("r")) {
            seed(world);
        }
    }

    void resize(World &, int, int) override {
        cols_ = 0; // Rebuilt at the next frame.
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = std::max(1, canvas.rows() - 1);
        if (cols_ != w || rows_ != h) {
            cols_ = w;
            rows_ = h;
            seed(world);
            stepped_ = now;
        }
        int steps = 0;
        while (stepped_ + Time(110) <= now && steps < 3) {
            step(world);
            stepped_ += Time(110);
            ++steps;
        }
        if (stepped_ + Time(110) <= now) {
            stepped_ = now;
        }
        draw(canvas);
    }

    bool finished() const override {
        return finished_;
    }

    std::string farewell() const override {
        return "Generation " + std::to_string(generation_) +
               ". Life goes on.\n";
    }

private:
    std::size_t idx(int x, int y) const {
        return std::size_t(y) * std::size_t(cols_) + std::size_t(x);
    }

    void seed(World &world) {
        age_.assign(std::size_t(cols_) * std::size_t(rows_), 0);
        for (auto &a : age_) {
            a = world.rng.below(100) < 28 ? 1 : 0;
        }
        generation_ = 0;
        quiet_ = 0;
    }

    void step(World &world) {
        std::vector<int> next(age_.size(), 0);
        int changed = 0;
        for (int y = 0; y < rows_; ++y) {
            for (int x = 0; x < cols_; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx || dy) {
                            const int nx = (x + dx + cols_) % cols_;
                            const int ny = (y + dy + rows_) % rows_;
                            n += age_[idx(nx, ny)] > 0;
                        }
                    }
                }
                const int a = age_[idx(x, y)];
                int v = 0;
                if (a > 0 && (n == 2 || n == 3)) {
                    v = std::min(a + 1, 40);
                }
                else if (a == 0 && n == 3) {
                    v = 1;
                }
                changed += (v > 0) != (a > 0);
                next[idx(x, y)] = v;
            }
        }
        age_ = std::move(next);
        ++generation_;
        // A board that settled into stillness gets shaken after a while.
        quiet_ = changed < std::max(2, cols_ * rows_ / 800) ? quiet_ + 1 : 0;
        if (quiet_ > 40) {
            seed(world);
        }
    }

    void draw(AsciiCanvas &canvas) const {
        for (int y = 0; y < rows_; ++y) {
            for (int x = 0; x < cols_; ++x) {
                const int a = age_[idx(x, y)];
                if (a == 0) {
                    canvas.put(x, y, U' ', {0, 0, 0}, {6, 8, 14});
                }
                else {
                    const double t = std::min(1.0, a / 20.0);
                    canvas.put(x,
                               y,
                               a < 3 ? U'o' : U'#',
                               mix({255, 255, 200}, {40, 190, 120}, t),
                               {6, 8, 14},
                               a < 3);
                }
            }
        }
        canvas.text(0,
                    rows_,
                    " life   q quit   space reseed   generation " +
                        std::to_string(generation_) + " ",
                    {200, 205, 215},
                    {24, 28, 40});
    }

    std::vector<int> age_;
    int cols_ = 0, rows_ = 0;
    int generation_ = 0, quiet_ = 0;
    Time stepped_{0};
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeLife(const std::vector<std::string> &) {
    return std::make_unique<Life>();
}

} // namespace fakeos
