#include "bigfont.h"
#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>

namespace fakeos {
namespace {

// A big clock that tells the (pretend) time, with a slowly moving rainbow and
// a colon that blinks. Space changes between 24 and 12 hours.
class Clock final : public Program {
public:
    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Escape) || (key.is(Key::Type::Ctrl) && key.text == "c") ||
            key.isText("q") || key.isText("Q")) {
            finished_ = true;
        }
        else if (key.isText(" ")) {
            twelve_ = !twelve_;
        }
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear({10, 10, 16});
        const std::int64_t seconds = world.unixNow();
        const int day = int(((seconds % 86400) + 86400) % 86400);
        int hour = day / 3600;
        const int minute = (day / 60) % 60, second = day % 60;
        const bool pm = hour >= 12;
        if (twelve_) {
            hour = hour % 12 == 0 ? 12 : hour % 12;
        }
        const std::string digits = std::string(1, char('0' + hour / 10)) + char('0' + hour % 10) +
                                   char('0' + minute / 10) + char('0' + minute % 10) +
                                   char('0' + second / 10) + char('0' + second % 10);
        // Three pairs of digits with two colons: 6 glyphs of 5, gaps of 1, colons of 3.
        const int total = 6 * 5 + 3 + 2 * 3;
        const bool big = w >= total + 2 && h >= 9;
        const double t = double(now.count()) / 1000.0;
        if (!big) {
            const std::string text = digits.substr(0, 2) + ":" + digits.substr(2, 2) + ":" + digits.substr(4, 2);
            canvas.text(std::max(0, (w - int(text.size())) / 2), h / 2, text, hsv(t * 0.05, 0.5, 1), {10, 10, 16}, true);
        }
        else {
            int x = (w - total) / 2;
            const int y = std::max(0, h / 2 - 3);
            for (int i = 0; i < 6; ++i) {
                const auto *glyph = bigGlyph(digits[std::size_t(i)]);
                for (int gy = 0; gy < 5; ++gy) {
                    for (int gx = 0; gx < 5; ++gx) {
                        if (glyph[gy][gx] == '#') {
                            canvas.put(x + gx, y + gy, U'#', hsv(t * 0.05 + (x + gx) * 0.01, 0.55, 1), {10, 10, 16}, true);
                        }
                    }
                }
                x += 5 + (i % 2 == 1 ? 0 : 1);
                if (i % 2 == 1 && i < 5) {
                    if (second % 2 == 0) { // The colon blinks.
                        canvas.put(x + 1, y + 1, U'#', {200, 200, 210}, {10, 10, 16}, true);
                        canvas.put(x + 1, y + 3, U'#', {200, 200, 210}, {10, 10, 16}, true);
                    }
                    x += 3;
                }
            }
        }
        const std::string note = std::string(twelve_ ? (pm ? "PM" : "AM") : "UTC") + "   q quit   space 12/24 h";
        canvas.text(std::max(0, (w - int(note.size())) / 2), h - 1, note, {150, 150, 165}, {10, 10, 16});
    }

    bool finished() const override {
        return finished_;
    }
    std::string farewell() const override {
        return "Time flies. You noticed.\n";
    }
    Time interval() const override {
        return Time(100);
    }

private:
    bool finished_ = false;
    bool twelve_ = false;
};

} // namespace

std::unique_ptr<Program> makeClock(const std::vector<std::string> &) {
    return std::make_unique<Clock>();
}

} // namespace fakeos
