#include "color.h"
#include "programs.h"

#include <algorithm>
#include <string>

namespace fakeos {
namespace {

// A machine that lost its disk: a black screen, a search for a boot device, and
// a message with a blinking cursor. It restarts by itself, or at a key once the
// message is up.
constexpr double kCorruptMs =
    2500; // The screen corrupts: glyphs swarm everywhere.
constexpr double kLookingAt =
    3200; // Then black; "Looking for boot media..." is typed.
constexpr double kMessageAt = 5200; // The failure message.
constexpr double kRestartAt = 9500; // The machine tries again by itself.
constexpr double kChurnMs = 60;     // How often the garbage changes.
constexpr double kTypeMsPerChar = 28;
constexpr int kBlinkMs = 500;

const char *const kLooking = "Looking for boot media...";
const char *const kMessage =
    "No bootable device -- insert boot disk and press any key";

class Crash final : public Program {
public:
    void begin(World &, Time now) override {
        start_ = now;
        lastNow_ = now;
    }
    void key(World &, const Key &key) override {
        if (key.is(Key::Type::Mouse)) {
            return;
        }
        // Keys mean nothing until the message is up.
        if (double((lastNow_ - start_).count()) >= kMessageAt + 300) {
            finished_ = true;
        }
    }
    void frame(World &world, Time now, AsciiCanvas &canvas) override {
        lastNow_ = now;
        const double t = double((now - start_).count());
        canvas.clear({0, 0, 0});
        if (t < kCorruptMs) {
            corrupt(world, canvas, t);
            return;
        }
        const Rgb grey{190, 190, 190};
        const int w = canvas.columns();
        int row = 0;
        if (t >= kLookingAt) {
            const int shown = typed(kLooking, t - kLookingAt);
            canvas.text(0,
                        row,
                        std::string(kLooking).substr(0, std::size_t(shown)),
                        grey);
            lastCount_ = beepFor(world, shown, lastCount_);
            if (shown < int(std::string(kLooking).size())) {
                cursor(canvas, shown, row, t);
            }
        }
        row = 2;
        if (t >= kMessageAt) {
            const std::string message = kMessage;
            const int shown = typed(kMessage, t - kMessageAt);
            canvas.text(0,
                        row,
                        message.substr(0, std::size_t(std::min<int>(shown, w))),
                        {230, 230, 230});
            messageCount_ = beepFor(world, shown, messageCount_);
            cursor(canvas,
                   std::min(shown, std::max(0, w - 1)),
                   row,
                   shown < int(message.size()) ? 0 : t);
        }
        else if (t >= kLookingAt + 900) {
            cursor(canvas, 0, row, t);
        }
        if (t >= kRestartAt) {
            finished_ = true;
        }
    }
    bool finished() const override {
        return finished_;
    }
    // A new world: the intro is the boot.
    void end(World &world) override {
        world.rebootRequest = true;
    }

private:
    // Garbage over the whole terminal: more and more cells with random glyphs
    // and colors, some inverted, changing a few times a second. It is drawn
    // with ordinary cursor addressing, so it works in any terminal.
    void corrupt(World &world, AsciiCanvas &canvas, double t) {
        if (!seeded_) {
            seeded_ = true;
            salt_ = std::uint32_t(world.rng.next() >> 40);
        }
        static const std::u32string glyphs =
            U"#%&*+=-:.<>?/\\|@$!~^0123456789ABCDEFXYZabcdefxyz";
        const double density = std::clamp(t / (kCorruptMs * 0.8), 0.04, 1.0);
        const auto frame = std::uint32_t(t / kChurnMs);
        for (int y = 0; y < canvas.rows(); ++y) {
            for (int x = 0; x < canvas.columns(); ++x) {
                if (cellNoise(x, y, salt_) >= density) {
                    continue;
                }
                const double pick = cellNoise(x, y, frame * 7919u + salt_);
                Rgb fg = hsv(cellNoise(y, x, frame + salt_),
                             0.7 + 0.3 * cellNoise(x + 1, y, frame),
                             0.9);
                Rgb bg{0, 0, 0};
                if (cellNoise(x, y + 3, frame + salt_) < 0.2) {
                    bg = fg; // Inverted now and then.
                    fg = {0, 0, 0};
                }
                canvas.put(x,
                           y,
                           glyphs[std::size_t(pick * double(glyphs.size()))],
                           fg,
                           bg,
                           pick > 0.9);
            }
        }
    }

    static int typed(const char *text, double ms) {
        return int(std::clamp(
            ms / kTypeMsPerChar, 0.0, double(std::string(text).size())));
    }
    // A pip for each letter typed (when sound is on).
    static int beepFor(World &world, int shown, int before) {
        if (shown > before) {
            world.beep(900 + double((shown * 61) % 300), 14, 0.3);
        }
        return shown;
    }
    static void cursor(AsciiCanvas &canvas, int x, int y, double t) {
        if (int(t / kBlinkMs) % 2 == 0) {
            canvas.put(x, y, U'_', {230, 230, 230}, {0, 0, 0}, true);
        }
    }

    Time start_{0}, lastNow_{0};
    bool seeded_ = false;
    std::uint32_t salt_ = 0;
    int lastCount_ = 0, messageCount_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeCrash(const std::vector<std::string> &) {
    return std::make_unique<Crash>();
}

} // namespace fakeos
