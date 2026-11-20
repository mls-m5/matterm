// mattrix: Mattrix OS, a pretend operating system, in the terminal it was
// started from. No window, no SDL, no renderer: the fakeos core and the raw
// terminal host only. It also runs inside matterm (`matterm -- ./mattrix`).

#include "system.h"
#include "ttyhost.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {
void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " [OPTIONS]\n\n"
        << "Mattrix OS 6.0 Professional Edition: a pretend Unix in your real "
           "terminal.\n"
           "Nothing in it touches your real computer. Type `help` inside.\n\n"
           "  --help, -h     Show this help.\n"
           "  --no-intro     Skip the intro animation.\n"
           "  --no-reveal    Print program output at once, not at a pace.\n"
           "  --cmd TEXT     Type and run TEXT at the first prompt.\n"
           "  --seed N       Seed for the animations (default: from the "
           "clock).\n\n"
           "Colors follow the terminal: truecolor with COLORTERM=truecolor, "
           "256 colors\n"
           "with TERM=...256color, otherwise the 16 basic ones. There is no "
           "sound.\n";
}
} // namespace

int main(int argc, char **argv) {
    fakeos::Config config;
    config.standalone = true;
    config.hostname = "mattrix";
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    config.seed = std::uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch)
            .count());
    config.unixTime = std::int64_t(
        std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count());
    config.revealCharsPerSecond = 1250; // About 13 lines a second.
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout, argv[0]);
            return 0;
        }
        else if (arg == "--no-intro") {
            config.intro = false;
        }
        else if (arg == "--no-reveal") {
            config.revealCharsPerSecond = 0;
        }
        else if (arg == "--cmd" && i + 1 < argc) {
            config.startCommand = argv[++i];
        }
        else if (arg == "--seed" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            std::uint64_t seed = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), seed);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size()) {
                std::cerr << "--seed requires a number\n";
                return 1;
            }
            config.seed = seed;
        }
        else {
            usage(std::cerr, argv[0]);
            return 1;
        }
    }
    return runTtyHost(config);
}
