#include "bigfont.h"
#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>

namespace fakeos {
namespace {

// Timing, in named constants.
constexpr double kSettleMs = 1000; // The word resolves out of noise.
constexpr double kGlowMs = 600;    // Then it takes its colors.
constexpr double kSubtitleStart = 1500;
constexpr double kSubtitleMsPerChar = 35;
constexpr double kFadeStart =
    3000; // The whole picture fades into the terminal.
constexpr double kFadeMs = 700;
constexpr double kSkipFadeMs = 300; // A key fades it out quicker.

// The name in block letters, resolved out of noise, then a subtitle, and a fade
// to the terminal's own background so the prompt appears without a jump. Any
// key or click starts the fade early.
class Intro final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
        lastNow_ = now;
    }
    void key(World &, const Key &key) override {
        if (leaving_) {
            return;
        }
        if (!key.is(Key::Type::Mouse) || key.press) {
            leaving_ = true; // Any key or click skips.
            leaveAt_ = lastNow_;
        }
    }
    bool wantsMouse() const override {
        return true;
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        lastNow_ = now;
        const double t = double((now - start_).count());
        const Rgb bg = world.background;
        // How much of the picture is left: 1 shows everything, 0 only the
        // background.
        double visible = 1.0;
        if (leaving_) {
            visible = 1.0 - double((now - leaveAt_).count()) / kSkipFadeMs;
        }
        else if (t >= kFadeStart) {
            visible = 1.0 - (t - kFadeStart) / kFadeMs;
        }
        visible = std::clamp(visible, 0.0, 1.0);
        const auto shade = [&](Rgb color) { return mix(bg, color, visible); };

        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear(bg);
        const std::string word = "MATTRIX";
        const int bannerWidth = int(word.size()) * 6 - 1;
        const bool big = w >= bannerWidth + 4 && h >= 11;
        const int left =
            (w - (big ? bannerWidth : int(word.size()) * 2 - 1)) / 2;
        const int top = std::max(0, h / 2 - 3);
        const double settle = std::clamp(t / kSettleMs, 0.0, 1.0);
        const std::string noiseSet = "#%&*+=-:.";
        const auto frameNumber = std::uint32_t(t / 70);
        if (big) {
            for (std::size_t li = 0; li < word.size(); ++li) {
                const auto *glyph = bigGlyph(word[li]);
                for (int gy = 0; gy < 5; ++gy) {
                    for (int gx = 0; gx < 5; ++gx) {
                        if (glyph[gy][gx] != '#') {
                            continue;
                        }
                        const int x = left + int(li) * 6 + gx, y = top + gy;
                        const bool done = cellNoise(x, y) < settle;
                        const Rgb color =
                            hsv(double(x) * 0.012 + t * 0.0003, 0.55, 1.0);
                        if (done) {
                            const double glow =
                                std::clamp((t - kSettleMs) / kGlowMs, 0.0, 1.0);
                            canvas.put(x,
                                       y,
                                       U'#',
                                       shade(mix({255, 255, 255}, color, glow)),
                                       bg,
                                       true);
                        }
                        else {
                            const auto pick = std::size_t(
                                cellNoise(x, y, frameNumber) * noiseSet.size());
                            canvas.put(x,
                                       y,
                                       char32_t(noiseSet[pick]),
                                       shade({90, 90, 110}),
                                       bg);
                        }
                    }
                }
            }
        }
        else {
            canvas.text(left,
                        top + 2,
                        "M A T T R I X",
                        shade(hsv(t * 0.0003, 0.5, 1)),
                        bg,
                        true);
        }
        const std::string subtitle = "a small pretend system. type 'help'.";
        const int chars =
            int(std::clamp((t - kSubtitleStart) / kSubtitleMsPerChar,
                           0.0,
                           double(subtitle.size())));
        canvas.text((w - int(subtitle.size())) / 2,
                    top + (big ? 7 : 4),
                    subtitle.substr(0, std::size_t(chars)),
                    shade({170, 170, 190}),
                    bg);
        canvas.text(std::max(0, (w - 22) / 2),
                    h - 1,
                    "press any key to skip",
                    shade({70, 70, 80}),
                    bg);
        if (visible <= 0.0) {
            finished_ = true;
        }
    }
    bool finished() const override {
        return finished_;
    }

private:
    Time start_{0}, lastNow_{0}, leaveAt_{0};
    bool leaving_ = false;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeIntro(const std::vector<std::string> &) {
    return std::make_unique<Intro>();
}

} // namespace fakeos
