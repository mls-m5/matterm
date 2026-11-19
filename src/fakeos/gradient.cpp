#include "color.h"
#include "eggs.h"
#include "programs.h"

#include <cmath>

namespace fakeos {
namespace {

// Slowly moving color fields drawn as background-colored cells.
class Gradient final : public Program {
public:
    explicit Gradient(const std::vector<std::string> &args) {
        for (const auto &arg : args) {
            pills_ = pills_ || arg == "--pills";
        }
    }
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
        else if (pills_ && (key.isText("b") || key.isText("B"))) {
            choice_ = "blue";
            finished_ = true;
        }
        else if (pills_ && (key.isText("r") || key.isText("R"))) {
            choice_ = "red";
            finished_ = true;
        }
    }
    bool wantsMouse() const override {
        return pills_;
    }
    // A choice made with the keyboard takes the same effect as a click on the
    // word (the egg table is the single source).
    void end(World &world) override {
        if (!choice_.empty()) {
            if (const auto *egg = matchClickEgg(world, "gradient", choice_)) {
                applyEgg(world, *egg, message_);
            }
        }
    }
    std::string farewell() const override {
        return message_;
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
        if (pills_) {
            drawPills(w, h, canvas);
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
    // The offer: two words to click (or press b / r).
    static void drawPills(int w, int h, AsciiCanvas &canvas) {
        const std::string title =
            "This is your last chance. After this, there is no turning back.";
        const std::string blue = "( blue )", red = "( red )";
        const std::string hint = "click a pill, or press b or r";
        const int width = std::min(w, int(title.size()) + 4);
        const int left = std::max(0, (w - width) / 2),
                  top = std::max(0, h / 2 - 3);
        const Rgb text{220, 220, 230}, back{12, 12, 16};
        for (int y = top; y < top + 7 && y < h; ++y) {
            for (int x = left; x < left + width && x < w; ++x) {
                canvas.put(x, y, U' ', text, back);
            }
        }
        canvas.text(left + 2,
                    top + 1,
                    title.substr(0, std::size_t(std::max(0, width - 4))),
                    text,
                    back);
        const int blueX = left + std::max(2, width / 2 - int(blue.size()) - 3);
        const int redX =
            left + std::min(width - int(red.size()) - 2, width / 2 + 3);
        canvas.text(blueX, top + 3, blue, {255, 255, 255}, {20, 60, 190}, true);
        canvas.text(redX, top + 3, red, {255, 255, 255}, {190, 25, 25}, true);
        canvas.text(left + std::max(2, (width - int(hint.size())) / 2),
                    top + 5,
                    hint,
                    {130, 130, 150},
                    back);
    }

    Time start_{0};
    bool pills_ = false;
    std::string choice_, message_;
    int mode_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeGradient(const std::vector<std::string> &args) {
    return std::make_unique<Gradient>(args);
}

} // namespace fakeos
