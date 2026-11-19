#include "color.h"
#include "programs.h"

#include <cmath>

namespace fakeos {
namespace {

// Slowly moving color fields drawn as background-colored cells.
class Gradient final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
    }
    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Escape) ||
            (key.is(Key::Type::Ctrl) && key.text == "c") || key.isText("q") ||
            key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText(" ")) {
            mode_ = (mode_ + 1) % 3;
        }
    }
    void frame(World &, Time now, AsciiCanvas &canvas) override {
        const double t = double((now - start_).count()) / 1000.0;
        const int w = canvas.columns(), h = canvas.rows();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const double u = double(x) / w, v = double(y) / h;
                double hue = 0;
                switch (mode_) {
                case 0:
                    hue = u * 0.8 + v * 0.3 + t * 0.05;
                    break;
                case 1:
                    hue = 0.5 +
                          0.5 * std::sin(u * 6 + t * 0.7) *
                              std::cos(v * 4 - t * 0.5) +
                          t * 0.02;
                    break;
                default:
                    hue = std::sin(std::hypot(u - 0.5, (v - 0.5) * 2) * 9 -
                                   t * 1.2) *
                              0.25 +
                          t * 0.03;
                    break;
                }
                canvas.put(x, y, U' ', {0, 0, 0}, hsv(hue, 0.7, 0.9 - 0.3 * v));
            }
        }
        canvas.text(1,
                    h - 1,
                    " gradient   q quit   space: next ",
                    {230, 230, 230},
                    {20, 20, 20});
    }
    bool finished() const override {
        return finished_;
    }
    Time interval() const override {
        return Time(50);
    }

private:
    Time start_{0};
    int mode_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeGradient(const std::vector<std::string> &) {
    return std::make_unique<Gradient>();
}

} // namespace fakeos
