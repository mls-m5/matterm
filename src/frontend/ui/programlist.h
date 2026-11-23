#pragma once

// "Start 3 programs again?": a question, the programs one per line (the list
// scrolls when it is long) and two buttons. "Not now" is the default and what
// Escape says (docs/tasks/restore-programs.md).
#include "dialogs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace frontend::ui {

class ProgramListDialog final : public Layer {
public:
    ProgramListDialog(std::string question,
                      std::vector<std::string> lines,
                      std::string startAction,
                      std::string skipAction)
        : question_(std::move(question))
        , lines_(std::move(lines))
        , startAction_(std::move(startAction))
        , skipAction_(std::move(skipAction)) {}

    // 0: Start them, 1: Not now.
    std::size_t selected() const {
        return selected_;
    }
    std::size_t firstLine() const {
        return first_;
    }

    CellSize wanted(int windowColumns, int windowRows) const override {
        CellSize s;
        s.columns =
            std::min(windowColumns, std::clamp(windowColumns - 4, 30, 72));
        const int list = std::min<int>(int(lines_.size()), 8);
        s.rows = std::min(
            windowRows,
            list + 6); // Frame, question, blank, list, blank, buttons, frame.
        return s;
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
        clampScroll();
    }

    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 12 || rows_ < 6)
            return;
        using namespace dialog_detail;
        frame(cells, columns_, rows_);
        cells.draw(2, 1, text(question_, columns_ - 4), kAccent, kBackground);
        const int visible = visibleLines();
        for (int i = 0; i < visible; ++i) {
            const std::size_t at = first_ + std::size_t(i);
            if (at >= lines_.size())
                break;
            cells.draw(
                2, 3 + i, text(lines_[at], columns_ - 5), kFg, kBackground);
        }
        if (lines_.size() >
            std::size_t(visible)) { // A mark that more is above or below.
            if (first_ > 0)
                cells.draw(columns_ - 3, 3, U"^", kDim, kBackground);
            if (first_ + std::size_t(visible) < lines_.size())
                cells.draw(columns_ - 3, 2 + visible, U"v", kDim, kBackground);
        }
        const auto spans = buttonSpans();
        static const char *const labels[2] = {"Start them", "Not now"};
        for (std::size_t i = 0; i < 2; ++i) {
            const bool on = i == selected_;
            std::u32string label = U"[ ";
            for (const char c : std::string(labels[i]))
                label += char32_t(static_cast<unsigned char>(c));
            label += U" ]";
            cells.draw(spans[i].first,
                       rows_ - 2,
                       label,
                       on ? kBackground : kFg,
                       on ? kAccent : kBackground);
        }
    }

    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl || chord.alt)
            return out;
        switch (chord.key) {
        case keys::Escape:
            return choose(1);
        case keys::Enter:
            return choose(selected_);
        case keys::Left:
            selected_ = 0;
            return out;
        case keys::Right:
            selected_ = 1;
            return out;
        case keys::Tab:
            selected_ = 1 - selected_;
            return out;
        case keys::Up:
            scroll(-1);
            return out;
        case keys::Down:
            scroll(1);
            return out;
        case keys::PageUp:
            scroll(-visibleLines());
            return out;
        case keys::PageDown:
            scroll(visibleLines());
            return out;
        default:
            return out;
        }
    }

    LayerOutcome click(int column, int row) override {
        LayerOutcome out;
        if (row != rows_ - 2)
            return out;
        const auto spans = buttonSpans();
        for (std::size_t i = 0; i < 2; ++i) {
            if (column >= spans[i].first && column < spans[i].second)
                return choose(i);
        }
        return out;
    }
    void wheel(double amount) override {
        scroll(amount > 0 ? -1 : 1);
    }

private:
    static std::u32string text(const std::string &s, int width) {
        std::u32string out;
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            out += (c < 0x20 || c >= 0x7f) ? U'?' : char32_t(c);
        }
        if (width > 0 && int(out.size()) > width) {
            out.resize(std::size_t(width - 1));
            out += U'~';
        }
        return out;
    }
    int visibleLines() const {
        return std::max(1, rows_ - 6);
    }
    void clampScroll() {
        const std::size_t visible = std::size_t(visibleLines());
        const std::size_t last =
            lines_.size() > visible ? lines_.size() - visible : 0;
        first_ = std::min(first_, last);
    }
    void scroll(int lines) {
        const std::size_t visible = std::size_t(visibleLines());
        const std::size_t last =
            lines_.size() > visible ? lines_.size() - visible : 0;
        const long next = long(first_) + lines;
        first_ = std::size_t(std::clamp<long>(next, 0, long(last)));
    }
    LayerOutcome choose(std::size_t index) const {
        LayerOutcome out;
        out.close = true;
        out.action = index == 0 ? startAction_ : skipAction_;
        return out;
    }
    std::vector<std::pair<int, int>> buttonSpans() const {
        const int a = 14, b = 11; // "[ Start them ]", "[ Not now ]"
        const int total = a + 2 + b;
        const int x = std::max(1, (columns_ - total) / 2);
        return {{x, x + a}, {x + a + 2, x + a + 2 + b}};
    }

    std::string question_;
    std::vector<std::string> lines_;
    std::string startAction_, skipAction_;
    std::size_t selected_ = 1; // Not now.
    std::size_t first_ = 0;
    int columns_ = 0;
    int rows_ = 0;
};

} // namespace frontend::ui
