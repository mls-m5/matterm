#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>

namespace fakeos {
namespace {

// "matterm" in a 5x5 block font, revealed out of noise, then a subtitle. Any
// key skips it.
const char *kGlyphs[5][5] = {
    // m
    {"#   #", "## ##", "# # #", "#   #", "#   #"},
    // a
    {" ### ", "#   #", "#####", "#   #", "#   #"},
    // t
    {"#####", "  #  ", "  #  ", "  #  ", "  #  "},
    // e
    {"#####", "#    ", "#### ", "#    ", "#####"},
    // r
    {"#### ", "#   #", "#### ", "#  # ", "#   #"},
};

int glyphIndex(char letter) {
    switch (letter) {
    case 'm':
        return 0;
    case 'a':
        return 1;
    case 't':
        return 2;
    case 'e':
        return 3;
    default:
        return 4;
    }
}

class Intro final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
    }
    void key(World &, const Key &key) override {
        if (!key.is(Key::Type::Mouse) || key.press) {
            finished_ = true; // Any key or click skips.
        }
    }
    bool wantsMouse() const override {
        return true;
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        const double t = double((now - start_).count());
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear();
        const std::string word = "matterm";
        const int bannerWidth = int(word.size()) * 6 - 1;
        const bool big = w >= bannerWidth + 4 && h >= 11;
        const int left =
            (w - (big ? bannerWidth : int(word.size()) * 2 - 1)) / 2;
        const int top = std::max(0, h / 2 - 3);
        const double settle = std::clamp(t / 1000.0, 0.0, 1.0);
        const std::string noiseSet = "#%&*+=-:.";
        const auto frameNumber = std::uint32_t(t / 70);
        if (big) {
            for (std::size_t li = 0; li < word.size(); ++li) {
                const auto &glyph = kGlyphs[glyphIndex(word[li])];
                for (int gy = 0; gy < 5; ++gy) {
                    for (int gx = 0; gx < 5; ++gx) {
                        if (glyph[gy][gx] != '#') {
                            continue;
                        }
                        const int x = left + int(li) * 6 + gx, y = top + gy;
                        const double reveal = cellNoise(x, y);
                        const bool done = reveal < settle;
                        const Rgb color =
                            hsv(double(x) * 0.012 + t * 0.0003, 0.55, 1.0);
                        if (done) {
                            const double glow =
                                std::clamp((t - 1000) / 600.0, 0.0, 1.0);
                            canvas.put(x,
                                       y,
                                       U'#',
                                       mix({255, 255, 255}, color, glow),
                                       {0, 0, 0},
                                       true);
                        }
                        else {
                            const auto pick = std::size_t(
                                cellNoise(x, y, frameNumber) * noiseSet.size());
                            canvas.put(x,
                                       y,
                                       char32_t(noiseSet[pick]),
                                       {90, 90, 110},
                                       {0, 0, 0});
                        }
                    }
                }
            }
        }
        else {
            const std::string spaced = "m a t t e r m";
            canvas.text(left,
                        top + 2,
                        spaced,
                        hsv(t * 0.0003, 0.5, 1),
                        {0, 0, 0},
                        true);
        }
        const std::string subtitle = "a small pretend system. type 'help'.";
        const int chars =
            int(std::clamp((t - 1500) / 35.0, 0.0, double(subtitle.size())));
        canvas.text((w - int(subtitle.size())) / 2,
                    top + (big ? 7 : 4),
                    subtitle.substr(0, std::size_t(chars)),
                    {170, 170, 190});
        canvas.text(std::max(0, (w - 22) / 2),
                    h - 1,
                    "press any key to skip",
                    {70, 70, 80});
        if (t >= 3300) {
            finished_ = true;
        }
        (void)world;
    }
    bool finished() const override {
        return finished_;
    }

private:
    Time start_{0};
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeIntro(const std::vector<std::string> &) {
    return std::make_unique<Intro>();
}

} // namespace fakeos
