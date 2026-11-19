#include "programs.h"

#include <algorithm>
#include <string>

namespace fakeos {
namespace {

// A machine that lost its disk: a black screen, a search for a boot device, and
// a message with a blinking cursor. It restarts by itself, or at a key once the
// message is up.
constexpr double kBlackMs = 700;    // Nothing at all.
constexpr double kLookingAt = 700;  // "Looking for boot media..." is typed.
constexpr double kMessageAt = 3200; // The failure message.
constexpr double kRestartAt = 8000; // The machine tries again by itself.
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
    int lastCount_ = 0, messageCount_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makeCrash(const std::vector<std::string> &) {
    return std::make_unique<Crash>();
}

} // namespace fakeos
