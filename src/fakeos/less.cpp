#include "programs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fakeos {
namespace {

// A pager: the text in World::programInput on the alternate screen.
class Pager final : public Program {
public:
    void begin(World &world, Time) override {
        std::string line;
        auto flush = [&] {
            lines_.push_back(line);
            line.clear();
        };
        bool escape = false;
        for (const char c : world.programInput) {
            if (escape) { // Drop color and cursor sequences from the text.
                if (c >= 0x40 && c <= 0x7e && c != '[') {
                    escape = false;
                }
                continue;
            }
            if (c == '\x1b') {
                escape = true;
            }
            else if (c == '\n') {
                flush();
            }
            else if (c == '\t') {
                line.append(8 - line.size() % 8, ' ');
            }
            else if (c != '\r') {
                line += c;
            }
        }
        if (!line.empty()) {
            flush();
        }
        world.programInput.clear();
        if (lines_.empty()) {
            lines_.push_back("(empty)");
        }
    }

    void key(World &, const Key &key) override {
        using T = Key::Type;
        if (key.is(T::Escape) || key.isText("q") || key.isText("Q") ||
            (key.is(T::Ctrl) && key.text == "c")) {
            finished_ = true;
        }
        else if (key.is(T::Down) || key.is(T::Enter) || key.isText("j")) {
            scroll(1);
        }
        else if (key.is(T::Up) || key.isText("k")) {
            scroll(-1);
        }
        else if (key.is(T::PageDown) || key.isText(" ") || key.isText("f")) {
            scroll(page_);
        }
        else if (key.is(T::PageUp) || key.isText("b")) {
            scroll(-page_);
        }
        else if (key.is(T::Home) || key.isText("g")) {
            top_ = 0;
        }
        else if (key.is(T::End) || key.isText("G")) {
            top_ = int(lines_.size());
            scroll(0);
        }
        else if (key.is(T::Mouse) && key.press) {
            if (key.button == 64) {
                scroll(-3);
            }
            else if (key.button == 65) {
                scroll(3);
            }
        }
    }

    bool wantsMouse() const override {
        return true;
    }

    void frame(World &, Time, AsciiCanvas &canvas) override {
        const int w = canvas.columns(), h = canvas.rows();
        page_ = std::max(1, h - 2);
        scroll(0); // Keep the position valid after a resize.
        canvas.clear();
        const Rgb text{210, 210, 210}, dim{80, 90, 110}, back{0, 0, 0};
        for (int y = 0; y < h - 1; ++y) {
            const int index = top_ + y;
            if (index < int(lines_.size())) {
                canvas.text(
                    0,
                    y,
                    lines_[std::size_t(index)].substr(0, std::size_t(w)),
                    text,
                    back);
            }
            else {
                canvas.text(0, y, "~", dim, back);
            }
        }
        const bool atEnd = top_ + (h - 1) >= int(lines_.size());
        canvas.text(
            0, h - 1, atEnd ? "(END)" : ":", {0, 0, 0}, {210, 210, 210});
    }

    bool finished() const override {
        return finished_;
    }
    Time interval() const override {
        return Time(100);
    }

private:
    void scroll(int delta) {
        const int visible = std::max(1, page_ + 1);
        const int last = std::max(0, int(lines_.size()) - visible);
        top_ = std::clamp(top_ + delta, 0, last);
    }

    std::vector<std::string> lines_;
    int top_ = 0;
    int page_ = 20;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<Program> makePager(const std::vector<std::string> &) {
    return std::make_unique<Pager>();
}

} // namespace fakeos
