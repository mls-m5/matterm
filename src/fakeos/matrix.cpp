#include "color.h"
#include "programs.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// The whole sequence, as data: how long each part lasts.
constexpr Time kBlack{900};        // The screen goes black.
constexpr int kTypeMs = 130;       // Per typed character.
constexpr Time kHelloPause{2200};  // "Hello, Neo." stays, the cursor blinks.
constexpr Time kRabbitPause{2400}; // "Follow the white rabbit." stays.
constexpr Time kRain{14000};       // The digital rain.
constexpr Time kFade{3500};        // No new drops; the last ones run out.
constexpr int kBlinkMs = 500;
constexpr int kStepMs = 33;

const char *const kHello = "Hello, Neo.";
const char *const kRabbit = "Follow the white rabbit.";

// The Matrix egg: black, typed words with a blinking cursor, digital rain, and
// then the intro again. A key skips the current part.
class Matrix final : public Program {
public:
    void begin(World &, Time now) override {
        phaseStart_ = now;
        stepped_ = now;
    }

    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Mouse) && (!key.press || key.motion)) {
            return;
        }
        next(lastNow_);
    }

    void resize(World &, int, int) override {
        drops_.clear();
        glyphs_.clear();
    }

    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        lastNow_ = now;
        const int w = canvas.columns(), h = canvas.rows();
        canvas.clear();
        const Time t = now - phaseStart_;
        switch (phase_) {
        case Phase::Black:
            if (t >= kBlack) {
                next(now);
            }
            break;
        case Phase::Hello:
            drawTyped(world, canvas, kHello, t, kHelloPause, w, h);
            if (t >= typedTime(kHello) + kHelloPause) {
                next(now);
            }
            break;
        case Phase::Rabbit:
            drawTyped(world, canvas, kRabbit, t, kRabbitPause, w, h);
            if (t >= typedTime(kRabbit) + kRabbitPause) {
                next(now);
            }
            break;
        case Phase::Rain:
            rain(world, canvas, now, t, w, h);
            break;
        case Phase::Done:
            break;
        }
    }

    bool finished() const override {
        return phase_ == Phase::Done;
    }

    // Back to the intro, which leads to the prompt again; the world keeps what
    // happened.
    void end(World &world) override {
        world.flags.insert("matrix-seen");
        world.launch = World::Launch{"intro", {}, {}};
    }

private:
    enum class Phase { Black, Hello, Rabbit, Rain, Done };

    void next(Time now) {
        phase_ = phase_ == Phase::Black    ? Phase::Hello
                 : phase_ == Phase::Hello  ? Phase::Rabbit
                 : phase_ == Phase::Rabbit ? Phase::Rain
                                           : Phase::Done;
        phaseStart_ = now;
        stepped_ = now;
        typedBefore_ = 0;
        if (phase_ == Phase::Rain) {
            drops_.clear();
        }
    }

    static Time typedTime(const std::string &text) {
        return Time(int(text.size()) * kTypeMs);
    }

    void drawTyped(World &world,
                   AsciiCanvas &canvas,
                   const std::string &text,
                   Time t,
                   Time pause,
                   int w,
                   int h) {
        const Rgb green{60, 255, 110};
        const int shown = int(std::clamp<long long>(
            t.count() / kTypeMs, 0, (long long)text.size()));
        const int x = std::max(0, (w - int(text.size())) / 2), y = h / 2;
        canvas.text(x, y, text.substr(0, std::size_t(shown)), green);
        if (shown > typedBefore_) { // A pip for each character typed.
            world.beep(1100 + double((shown * 53) % 300), 22, 0.3);
        }
        typedBefore_ = shown;
        // The cursor blinks while waiting and holds still while typing.
        const bool typing = shown < int(text.size());
        const bool lit = typing || (t.count() / kBlinkMs) % 2 == 0;
        (void)pause;
        if (lit) {
            canvas.put(x + shown, y, U'_', green, {0, 0, 0}, true);
        }
    }

    struct Drop {
        double head = 0, speed = 0;
        int length = 0;
        Time wakes{0}; // A column rests until this time.
        bool active = false;
    };

    void rain(
        World &world, AsciiCanvas &canvas, Time now, Time t, int w, int h) {
        if (drops_.size() != std::size_t(w) ||
            glyphs_.size() != std::size_t(w) * std::size_t(h)) {
            drops_.assign(std::size_t(w), Drop{});
            glyphs_.assign(std::size_t(w) * std::size_t(h), 'A');
            static const char set[] =
                "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.=*+-<>|";
            for (auto &g : glyphs_) {
                g = set[world.rng.below(sizeof set - 1)];
            }
            for (auto &d : drops_) {
                d.wakes = now + Time(int(world.rng.below(2500)));
            }
        }
        const bool spawning = t < kRain;
        static const char set[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.=*+-<>|";
        while (stepped_ + Time(kStepMs) <= now) {
            stepped_ += Time(kStepMs);
            const double dt = kStepMs / 1000.0;
            for (auto &d : drops_) {
                if (!d.active) {
                    if (spawning && stepped_ >= d.wakes) {
                        d.active = true;
                        d.head = -double(world.rng.below(8));
                        d.speed = 8.0 + world.rng.unit() * 22.0;
                        d.length =
                            6 +
                            int(world.rng.below(std::uint32_t(std::max(2, h))));
                        d.length = std::min(d.length, h + 4);
                    }
                    continue;
                }
                d.head += d.speed * dt;
                if (d.head - d.length > h) {
                    d.active = false;
                    d.wakes = stepped_ + Time(int(world.rng.below(1800)));
                }
            }
            // A few glyphs change each step, so the rain shimmers.
            for (int i = 0; i < std::max(1, w * h / 60); ++i) {
                glyphs_[world.rng.below(std::uint32_t(glyphs_.size()))] =
                    set[world.rng.below(sizeof set - 1)];
            }
        }
        bool any = false;
        for (int x = 0; x < w; ++x) {
            const auto &d = drops_[std::size_t(x)];
            if (!d.active) {
                continue;
            }
            any = true;
            for (int k = 0; k < d.length; ++k) {
                const int y = int(std::floor(d.head)) - k;
                if (y < 0 || y >= h) {
                    continue;
                }
                const double life = 1.0 - double(k) / d.length;
                Rgb color;
                bool bold = false;
                if (k == 0) {
                    color = {215, 255, 225}; // The bright head.
                    bold = true;
                }
                else {
                    const double g = std::pow(life, 1.4);
                    color = {std::uint8_t(20 * g),
                             std::uint8_t(70 + 185 * g),
                             std::uint8_t(30 + 50 * g)};
                }
                canvas.put(x,
                           y,
                           char32_t(glyphs_[std::size_t(y) * std::size_t(w) +
                                            std::size_t(x)]),
                           color,
                           {0, 0, 0},
                           bold);
            }
        }
        if (t >= kRain + kFade || (!spawning && !any)) {
            next(now);
        }
    }

    Phase phase_ = Phase::Black;
    Time phaseStart_{0}, stepped_{0}, lastNow_{0};
    int typedBefore_ = 0; // Characters already typed (and pipped) in this part.
    std::vector<Drop> drops_;
    std::vector<char> glyphs_;
};

} // namespace

std::unique_ptr<Program> makeMatrix(const std::vector<std::string> &) {
    return std::make_unique<Matrix>();
}

} // namespace fakeos
