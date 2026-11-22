// TerminalModel steady-state cost and resize ("drag") cost. Build in Release.
//   matterm-model-bench            writes, scrolls, then drags
// The same file builds against a tree without reflow (no MATTERM_HAS_REFLOW): the drag numbers are
// then the cost of the clip-only rebuild.
#include "terminalmodel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::vector<std::u32string> makeLines(std::size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::u32string> lines;
    for (std::size_t i = 0; i < count; ++i) {
        std::u32string l;
        const std::size_t n = rng() % 6 == 0 ? 0 : 1 + rng() % 150;
        for (std::size_t k = 0; k < n; ++k) l += rng() % 7 == 0 ? U' ' : char32_t(U'a' + rng() % 26);
        lines.push_back(std::move(l));
    }
    return lines;
}

double writeBench(std::size_t columns, const std::vector<std::u32string> &lines, int repeat) {
    double best = 1e18;
    for (int r = 0; r < repeat; ++r) {
        TerminalModel m(columns, 50, 10000);
        const auto t0 = Clock::now();
        for (int pass = 0; pass < 6; ++pass) {
            for (const auto &l : lines) {
                m.write(l);
                m.carriageReturn();
                m.lineFeed();
            }
        }
        best = std::min(best, ms(Clock::now() - t0));
    }
    return best;
}

struct Drag {
    double worst = 0, mean = 0, settle = 0;
};
Drag dragBench(std::size_t historyLines, std::size_t from, std::size_t to) {
    const auto lines = makeLines(historyLines, 5);
    TerminalModel m(from, 50, 20000);
    for (const auto &l : lines) {
        m.write(l);
        m.carriageReturn();
        m.lineFeed();
    }
    std::vector<double> steps;
    for (std::size_t w = from; w != to; w += (to > from ? 1 : std::size_t(-1))) {
        const auto t0 = Clock::now();
        m.resize(w, 50);
        m.advanceViewport(0.016);
        steps.push_back(ms(Clock::now() - t0));
    }
    Drag d;
    for (const double s : steps) {
        d.worst = std::max(d.worst, s);
        d.mean += s / double(steps.size());
    }
    const auto t0 = Clock::now();
#ifdef MATTERM_HAS_REFLOW
    m.finishReflow();
#endif
    d.settle = ms(Clock::now() - t0);
    return d;
}
} // namespace

int main() {
    const auto lines = makeLines(4000, 1);
    std::printf("steady state (ms for 24000 lines through a 10000-row history, best of 5)\n");
    for (const std::size_t columns : {80u, 200u}) {
        std::printf("  write, %3zu columns: %8.2f ms\n", columns, writeBench(columns, lines, 5));
    }
    std::printf("drag (history lines, step 1 column)\n");
    for (const std::size_t n : {1000u, 5000u, 10000u}) {
        const auto narrow = dragBench(n, 200, 80);
        const auto wide = dragBench(n, 80, 200);
        std::printf("  %5zu lines  narrow 200->80: step mean %7.3f worst %7.3f settle %8.2f | widen 80->200: mean %7.3f worst %7.3f settle %8.2f ms\n",
                    n, narrow.mean, narrow.worst, narrow.settle, wide.mean, wide.worst, wide.settle);
    }
}
