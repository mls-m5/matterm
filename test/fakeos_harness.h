#pragma once

#include "system.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <optional>
#include <string>

namespace fakeos_test {
using namespace fakeos;

// A pretend system wired to a real parser and model, with a fake clock.
struct Pc {
    explicit Pc(bool intro = false,
                int columns = 80,
                int rows = 24,
                std::uint64_t seed = 1)
        : sys(makeConfig(intro, columns, rows, seed))
        , model(columns, rows, 200)
        , parser(model) {
        apply(sys.start());
        advance(0);
    }
    // A system with a prepared configuration (its size is that of the model).
    explicit Pc(Config config)
        : sys(config)
        , model(config.columns, config.rows, 200)
        , parser(model) {
        apply(sys.start());
        advance(0);
    }
    static Config makeConfig(bool intro,
                             int columns,
                             int rows,
                             std::uint64_t seed) {
        Config c;
        c.intro = intro;
        c.columns = columns;
        c.rows = rows;
        c.seed = seed;
        return c;
    }
    void apply(const Reaction &r) {
        raw += r.output;
        parser.feed(r.output);
        if (r.exitCode) {
            exitCode = r.exitCode;
        }
    }
    void type(const std::string &bytes) {
        apply(sys.input(bytes));
    }
    void run(const std::string &line) {
        type(line + "\r");
    }
    void advance(long long ms) {
        now += ms;
        apply(sys.tick(Time(now)));
    }
    std::string row(std::size_t y) const {
        std::string text;
        for (std::size_t x = 0; x < model.columns(); ++x) {
            const auto cp = model.cell(x, y).codePoint;
            text.push_back(cp >= 32 && cp < 127 ? char(cp) : ' ');
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }
    std::string screen() const {
        std::string text;
        for (std::size_t y = 0; y < model.rows(); ++y) {
            text += row(y) + "\n";
        }
        return text;
    }
    bool shows(const std::string &needle) const {
        return screen().find(needle) != std::string::npos;
    }
    // The last non-empty row (the prompt, normally).
    std::string last() const {
        for (std::size_t y = model.rows(); y-- > 0;) {
            if (!row(y).empty()) {
                return row(y);
            }
        }
        return {};
    }

    long long now = 1000;
    System sys;
    TerminalModel model;
    VtParser parser;
    std::string raw;
    std::optional<int> exitCode;
};
} // namespace fakeos_test
