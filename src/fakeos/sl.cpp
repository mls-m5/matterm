#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// For everyone who types sl when they meant ls: a steam locomotive crosses the
// terminal with a trail of smoke. Any key lets it go early.
class Train final : public Program {
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
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear({0, 0, 0});
        const int trainWidth = width();
        // Enters at the right edge and leaves past the left one.
        const long long speedMsPerCell = 38;
        const int x = w - int((now - start_).count() / speedMsPerCell);
        const int top = std::max(0, h / 2 - int(kTrain.size()) / 2);
        // Smoke puffs rise from the chimney and drift backwards.
        for (int i = 0; i < 9; ++i) {
            const int px = x + 7 + i * 3 + int(world.rng.below(2));
            const int py = top - 1 - i / 2;
            const int shade = 210 - i * 18;
            canvas.put(
                px,
                py,
                i % 2 ? U'o' : U'O',
                {std::uint8_t(shade), std::uint8_t(shade), std::uint8_t(shade)},
                {0, 0, 0});
        }
        for (std::size_t r = 0; r < kTrain.size(); ++r) {
            const std::string row = kTrain[r];
            for (int c = 0; c < int(row.size()); ++c) {
                const int cx = x + c;
                if (row[std::size_t(c)] != ' ') {
                    const bool wheel = r + 1 >= kTrain.size() - 1;
                    canvas.put(cx,
                               top + int(r),
                               char32_t(row[std::size_t(c)]),
                               wheel ? Rgb{190, 190, 200} : Rgb{235, 170, 70},
                               {0, 0, 0});
                }
            }
        }
        // Rails.
        for (int cx = 0; cx < w; ++cx) {
            canvas.put(cx,
                       top + int(kTrain.size()),
                       (cx + int((now - start_).count() / 100)) % 4 ? U'='
                                                                    : U'+',
                       {120, 120, 130},
                       {0, 0, 0});
        }
        if (x + trainWidth < 0) {
            finished_ = true;
        }
    }

    bool finished() const override {
        return finished_;
    }

    std::string farewell() const override {
        return "Choo choo. You meant ls, didn't you?\n";
    }

    Time interval() const override {
        return Time(40);
    }

private:
    static int width() {
        std::size_t w = 0;
        for (const auto &r : kTrain) {
            w = std::max(w, r.size());
        }
        return int(w);
    }

    static inline const std::vector<std::string> kTrain = {
        "      ====        ________                ___________ ",
        "  _D _|  |_______/        \\__I_I_____===__|_________| ",
        "   |(_)---  |   H\\________/ |   |        =|___ ___|   ",
        "   /     |  |   H  |  |     |   |         ||_| |_||   ",
        "  |      |  |   H  |__--------------------| [___] |   ",
        "  | ________|___H__/__|_____/[][]~\\_______|       |   ",
        "  |/ |   |-----------I_____I [][] []  D   |=======|__ ",
        "__/ =| o |=-~~\\  /~~\\  /~~\\  /~~\\ ____Y___________|__ ",
        " |/-=|___|=O=====O=====O=====O   |_____/~\\___/        ",
        "  \\_/      \\__/  \\__/  \\__/  \\__/      \\_/            ",
    };

    Time start_{0};
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeTrain(const std::vector<std::string> &) {
    return std::make_unique<Train>();
}

} // namespace fakeos
