#include "bigfont.h"
#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// The whole intro, as named timings (milliseconds from its start).
constexpr double kSettleMs = 900;  // The name resolves out of noise.
constexpr double kGlowMs = 500;    // Then it takes its colors.
constexpr double kTagline1 = 1300; // First line is typed.
constexpr double kTypeMsPerChar = 32;
constexpr double kScramble =
    3500; // It scrambles and vanishes, letter by letter.
constexpr double kScrambleSpread = 600; // Letters start at different moments...
constexpr double kGlitchMs = 350;       // ...and show noise for this long.
constexpr double kTagline2 = 4800;      // Second line is typed.
constexpr double kDemoOn = 900;         // "DEMO MODE" blinks in the corner,
constexpr double kDemoOff = 6000;       // for a few seconds, and goes away.
constexpr double kBlinkMs = 400;
constexpr double kBackgroundStart = 5200; // The background moves to the real
constexpr double kBackgroundMs = 1800;    // terminal's, gradually.
constexpr double kTextFadeStart = 6700;   // Last, the text lets go.
constexpr double kTextFadeMs = 300;
constexpr double kSkipTextMs = 300; // A key: the text goes quickly...
constexpr double kSkipBackgroundMs =
    1200; // ...the background still moves smoothly.

const char *const kLine1 = "Futuristic system agnostic terminal emulator";
const char *const kLine2 = "Real terminal, fake operating system";
const Rgb kIntroBackground{4, 6, 14}; // Dark, before the terminal's own color.

struct Placed {
    int x, y;
    char c;
};

// A line of text centered in the width, wrapped at spaces when it is too long.
std::vector<Placed> layout(const std::string &text, int width, int firstRow) {
    std::vector<std::string> lines;
    const std::size_t limit = std::size_t(std::max(8, width - 2));
    std::string line;
    std::string word;
    auto flushWord = [&] {
        if (word.empty()) {
            return;
        }
        if (!line.empty() && line.size() + 1 + word.size() > limit) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (const char c : text) {
        if (c == ' ') {
            flushWord();
        }
        else {
            word += c;
        }
    }
    flushWord();
    if (!line.empty()) {
        lines.push_back(line);
    }
    std::vector<Placed> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const int x = (width - int(lines[i].size())) / 2;
        for (std::size_t k = 0; k < lines[i].size(); ++k) {
            out.push_back({x + int(k), firstRow + int(i), lines[i][k]});
        }
    }
    return out;
}

class Intro final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
        lastNow_ = now;
    }
    void key(World &, const Key &key) override {
        if (leaving_ || (key.is(Key::Type::Mouse) && !key.press)) {
            return;
        }
        // Any key or click: the text goes quickly, the background keeps its
        // smooth move to the terminal (it never cuts).
        leaving_ = true;
        leaveAt_ = lastNow_;
        const double t = double((lastNow_ - start_).count());
        alphaAtLeave_ = alpha(t);
        backgroundAtLeave_ = backgroundAmount(t);
    }
    bool wantsMouse() const override {
        return true;
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        lastNow_ = now;
        const double t = double((now - start_).count());
        double visible, mixToTerminal;
        if (leaving_) {
            const double since = double((now - leaveAt_).count());
            visible =
                alphaAtLeave_ * std::clamp(1.0 - since / kSkipTextMs, 0.0, 1.0);
            mixToTerminal = backgroundAtLeave_ +
                            (1.0 - backgroundAtLeave_) *
                                std::clamp(since / kSkipBackgroundMs, 0.0, 1.0);
        }
        else {
            visible = alpha(t);
            mixToTerminal = backgroundAmount(t);
        }
        const Rgb bg = mix(kIntroBackground, world.background, mixToTerminal);
        const auto shade = [&](Rgb color) { return mix(bg, color, visible); };
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear(bg);

        drawBanner(canvas, t, w, h, bg, shade);
        const int textRow =
            std::max(0, h / 2 - 3) + (w >= 45 && h >= 11 ? 7 : 4);
        drawTagline1(world, canvas, t, w, textRow, bg, shade);
        drawTagline2(world, canvas, t, w, textRow, bg, shade);
        // The red notice blinks for a few seconds in the corner.
        if (t >= kDemoOn && t < kDemoOff &&
            int((t - kDemoOn) / kBlinkMs) % 2 == 0) {
            const std::string notice = " DEMO MODE ";
            canvas.text(std::max(0, w - int(notice.size()) - 1),
                        0,
                        notice,
                        shade({255, 60, 60}),
                        bg,
                        true);
        }
        canvas.text(std::max(0, (w - 22) / 2),
                    h - 1,
                    "press any key to skip",
                    shade({70, 70, 80}),
                    bg);
        if (visible <= 0.0 && mixToTerminal >= 1.0) {
            finished_ = true;
        }
    }

    bool finished() const override {
        return finished_;
    }

private:
    static double alpha(double t) {
        return std::clamp(1.0 - (t - kTextFadeStart) / kTextFadeMs, 0.0, 1.0);
    }
    static double backgroundAmount(double t) {
        return std::clamp((t - kBackgroundStart) / kBackgroundMs, 0.0, 1.0);
    }

    template <typename Shade>
    void drawBanner(AsciiCanvas &canvas,
                    double t,
                    int w,
                    int h,
                    Rgb bg,
                    Shade shade) const {
        const std::string word = "MATTERM";
        const int bannerWidth = int(word.size()) * 6 - 1;
        const bool big = w >= bannerWidth + 4 && h >= 11;
        const int left =
            (w - (big ? bannerWidth : int(word.size()) * 2 - 1)) / 2;
        const int top = std::max(0, h / 2 - 3);
        const double settle = std::clamp(t / kSettleMs, 0.0, 1.0);
        const std::string noiseSet = "#%&*+=-:.";
        const auto frameNumber = std::uint32_t(t / 70);
        if (!big) {
            canvas.text(left,
                        top + 2,
                        "M A T T E R M",
                        shade(hsv(t * 0.0003, 0.5, 1)),
                        bg,
                        true);
            return;
        }
        for (std::size_t li = 0; li < word.size(); ++li) {
            const auto *glyph = bigGlyph(word[li]);
            for (int gy = 0; gy < 5; ++gy) {
                for (int gx = 0; gx < 5; ++gx) {
                    if (glyph[gy][gx] != '#') {
                        continue;
                    }
                    const int x = left + int(li) * 6 + gx, y = top + gy;
                    if (cellNoise(x, y) < settle) {
                        const double glow =
                            std::clamp((t - kSettleMs) / kGlowMs, 0.0, 1.0);
                        const Rgb color =
                            hsv(double(x) * 0.012 + t * 0.0003, 0.55, 1.0);
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

    // First line: typed, held, then each letter turns to noise and vanishes.
    template <typename Shade>
    void drawTagline1(World &world,
                      AsciiCanvas &canvas,
                      double t,
                      int w,
                      int row,
                      Rgb bg,
                      Shade shade) {
        if (t < kTagline1) {
            return;
        }
        const auto cells = layout(kLine1, w, row);
        const int typed = int((t - kTagline1) / kTypeMsPerChar);
        const int shown = std::min<int>(typed, int(cells.size()));
        if (shown >
            typed1_) { // A pip for the letters typed since the last frame.
            world.beep(1000 + double((shown * 47) % 360), 18, 0.25);
        }
        typed1_ = shown;
        static const std::string noise = "#%&*+=-:.<>?/\\|";
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (int(i) >= typed) {
                break;
            }
            const auto &cell = cells[i];
            Rgb color = shade({170, 190, 230});
            char c = cell.c;
            if (t >= kScramble) {
                const double local =
                    t - kScramble - cellNoise(int(i), 3, 7) * kScrambleSpread;
                if (local >= kGlitchMs) {
                    continue; // Gone.
                }
                if (local >= 0) {
                    const auto frame = std::uint32_t(t / 60);
                    c = noise[std::size_t(cellNoise(int(i), 5, frame) *
                                          noise.size())];
                    color = shade({120, 255, 160});
                }
            }
            canvas.put(cell.x, cell.y, char32_t(c), color, bg);
        }
    }

    template <typename Shade>
    void drawTagline2(World &world,
                      AsciiCanvas &canvas,
                      double t,
                      int w,
                      int row,
                      Rgb bg,
                      Shade shade) {
        if (t < kTagline2) {
            return;
        }
        const auto cells = layout(kLine2, w, row);
        const int typed = int((t - kTagline2) / kTypeMsPerChar);
        const int shown = std::min<int>(typed, int(cells.size()));
        if (shown > typed2_) {
            world.beep(1100 + double((shown * 41) % 330), 18, 0.28);
        }
        typed2_ = shown;
        for (std::size_t i = 0; i < cells.size() && int(i) < typed; ++i) {
            canvas.put(cells[i].x,
                       cells[i].y,
                       char32_t(cells[i].c),
                       shade({220, 230, 255}),
                       bg,
                       true);
        }
    }

    Time start_{0}, lastNow_{0}, leaveAt_{0};
    bool leaving_ = false;
    int typed1_ = 0, typed2_ = 0; // Letters typed so far, for the pips.
    double alphaAtLeave_ = 1.0, backgroundAtLeave_ = 0.0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeIntro(const std::vector<std::string> &) {
    return std::make_unique<Intro>();
}

} // namespace fakeos
