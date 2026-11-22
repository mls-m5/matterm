// Cost of reflowing the saved history on a width change (docs/SCROLLING_AND_REFLOW.md): a
// prototype of the algorithm on the real storage (TerminalMemory rows plus the model's
// per-row soft-wrap flags), not wired into the terminal model.
//   matterm-reflow-bench [history rows] [columns] [new columns]
#include "terminalmemory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct History {
    History(std::size_t columns, std::size_t capacity)
        : memory(std::make_unique<TerminalMemory>(columns, capacity)) {}
    std::unique_ptr<TerminalMemory> memory; // Not movable itself.
    std::vector<bool> wrap; // wrap[i]: row i continues on row i + 1 (the model's softWrap).
};

bool blank(const FChar &c) {
    const FChar d;
    return c.codePoint == d.codePoint && c.foreground.r == d.foreground.r && c.background.r == d.background.r &&
           c.attributes == d.attributes;
}

// Join rows across soft wraps, drop the trailing default blanks of each logical line (padding:
// the rows have no written extent), lay the line out again at `columns`.
History reflow(const History &old, std::size_t columns, std::size_t capacity) {
    History out(columns, capacity);
    std::vector<FChar> line;
    std::vector<FChar> chunk(columns);
    const std::size_t n = old.memory->size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto row = old.memory->row(i);
        line.insert(line.end(), row.data(), row.data() + row.size());
        if (old.wrap[i] && i + 1 < n) continue;
        std::size_t length = line.size();
        while (length > 0 && blank(line[length - 1])) --length;
        if (length == 0) {
            out.memory->append(FStringView(chunk.data(), 0)); // One blank row.
            out.wrap.push_back(false);
        }
        for (std::size_t at = 0; at < length; at += columns) {
            const std::size_t part = std::min(columns, length - at);
            out.memory->append(FStringView(line.data() + at, part));
            out.wrap.push_back(at + columns < length);
        }
        line.clear();
    }
    return out;
}

bool sameText(const History &a, const History &b) {
    if (a.memory->size() != b.memory->size()) return false;
    for (std::size_t i = 0; i < a.memory->size(); ++i) {
        const auto x = a.memory->row(i), y = b.memory->row(i);
        if (x.size() != y.size()) return false;
        for (std::size_t c = 0; c < x.size(); ++c)
            if (x.data()[c].codePoint != y.data()[c].codePoint) return false;
        if (a.wrap[i] != b.wrap[i]) return false;
    }
    return true;
}

// Logical lines as a shell session produces them: many short, some about a screen wide,
// a few much longer (wrapped by the terminal at `columns`).
History make(std::size_t rows, std::size_t columns, const char *mix) {
    History h(columns, rows);
    std::size_t seed = 12345;
    const auto next = [&] { return seed = seed * 6364136223846793005ull + 1442695040888963407ull; };
    std::vector<FChar> text;
    while (h.memory->size() < rows) {
        const auto r = (next() >> 33) % 100;
        std::size_t length;
        if (std::string(mix) == "long") length = columns / 2 + (next() >> 33) % (columns * 3);
        else if (std::string(mix) == "short") length = 1 + (next() >> 33) % (columns - 1);
        else length = r < 70 ? 1 + (next() >> 33) % (columns / 2) : r < 95 ? columns / 2 + (next() >> 33) % columns : columns + (next() >> 33) % (columns * 4);
        text.assign(length, FChar{});
        for (std::size_t i = 0; i < length; ++i) {
            text[i].codePoint = char32_t('a' + (next() >> 33) % 26);
            if ((i / 7) % 5 == 0) text[i].foreground = {200, 80, 80};
        }
        for (std::size_t at = 0; at < length && h.memory->size() < rows; at += columns) {
            const std::size_t part = std::min(columns, length - at);
            std::vector<FChar> padded(columns);
            std::copy(text.begin() + std::ptrdiff_t(at), text.begin() + std::ptrdiff_t(at + part), padded.begin());
            h.memory->append(FStringView(padded.data(), columns));
            h.wrap.push_back(at + columns < length);
        }
    }
    h.wrap.back() = false;
    return h;
}
} // namespace

int main(int argc, char **argv) {
    const auto arg = [&](int i, std::size_t def) {
        return argc > i ? std::size_t(std::strtoul(argv[i], nullptr, 10)) : def;
    };
    const std::size_t rows = arg(1, 1000), columns = arg(2, 200), narrow = arg(3, 100);
    for (const char *mix : {"short", "mixed", "long"}) {
        const auto base = make(rows, columns, mix);
        const std::size_t cap = rows * 4;
        auto t0 = Clock::now();
        History a = reflow(base, narrow, cap);
        auto t1 = Clock::now();
        History b = reflow(a, columns, cap);
        auto t2 = Clock::now();
        // Reflow of the narrow history back must give the original text and wraps.
        const bool same = sameText(base, b);
        // A drag: one column per step down to a third of the width and back.
        const std::size_t low = columns / 3;
        History cur = reflow(base, columns, cap);
        auto t3 = Clock::now();
        int steps = 0;
        for (std::size_t w = columns - 1; w >= low; --w, ++steps) cur = reflow(cur, w, cap);
        for (std::size_t w = low + 1; w <= columns; ++w, ++steps) cur = reflow(cur, w, cap);
        auto t4 = Clock::now();
        std::printf("%-5s rows %zu x %zu: narrow to %zu -> %zu rows %.3f ms, widen back %.3f ms (restored exactly: %s), "
                    "drag %d steps %.3f ms/step\n",
                    mix, base.memory->size(), columns, narrow, a.memory->size(), ms(t0, t1), ms(t1, t2),
                    same ? "yes" : "NO", steps, ms(t3, t4) / steps);
    }
}
