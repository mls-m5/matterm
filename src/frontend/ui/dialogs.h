#pragma once

// Three small overlays for the native app and the browser (docs/TABS_PLAN.md
// sections 8 and 9), all `Layer`s with no drawing backend:
//   ConfirmDialog  a question and a row of buttons ("Close" / "Cancel"; the third answer
//                  "Run in background" is added as one more button when it exists)
//   TextPrompt     one line of text ("Save session as...", "Rename tab...")
//   PickList       a list to choose from ("Open session...")
// What they answer is `LayerOutcome::action`: a button's own action id, or, for the prompt, the
// prefix followed by the text, and for the list the item's action id.
#include "layer.h"
#include "listview.h"
#include "textinput.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace frontend::ui {

namespace dialog_detail {
inline constexpr Rgb8 kBorder{62, 84, 128};
inline constexpr Rgb8 kBackground{28, 32, 40};
inline constexpr Rgb8 kSelected{52, 66, 98};
inline constexpr Rgb8 kFg{230, 230, 230};
inline constexpr Rgb8 kDim{140, 148, 160};
inline constexpr Rgb8 kAccent{120, 190, 255};

inline void frame(CellBuffer &cells, int columns, int rows) {
    cells.clear(FChar{U' ', kFg, kBorder});
    for (int y = 1; y < rows - 1; ++y) {
        for (int x = 1; x < columns - 1; ++x) cells.at(std::size_t(x), std::size_t(y)) = FChar{U' ', kFg, kBackground};
    }
}

// Words wrapped to `width` cells, long words cut; ASCII and UTF-8 bytes are treated one per cell
// only for ASCII, anything else is shown as '?' (the text here is built by the app, but a program
// name inside it is not trusted to be printable).
inline std::vector<std::u32string> wrap(const std::string &text, int width) {
    std::vector<std::u32string> lines;
    std::u32string line;
    std::u32string word;
    const auto flushWord = [&] {
        if (word.empty()) return;
        while (int(word.size()) > width) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, std::size_t(width)));
            word.erase(0, std::size_t(width));
        }
        if (!line.empty() && int(line.size() + 1 + word.size()) > width) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += U' ';
        line += word;
        word.clear();
    };
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == ' ') flushWord();
        else word += (c < 0x20 || c >= 0x7f) ? U'?' : char32_t(c);
    }
    flushWord();
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}
} // namespace dialog_detail

class ConfirmDialog final : public Layer {
public:
    struct Button {
        std::string label;
        std::string action; // LayerOutcome::action when chosen
    };
    // `cancel` is the index of the button Escape and an outside dismissal stand for; `initial`
    // the button Enter chooses first (the safe one for a destructive question).
    ConfirmDialog(std::string question, std::vector<Button> buttons, std::size_t cancel, std::size_t initial)
        : question_(std::move(question))
        , buttons_(std::move(buttons))
        , cancel_(cancel)
        , selected_(initial) {
        if (selected_ >= buttons_.size()) selected_ = buttons_.empty() ? 0 : buttons_.size() - 1;
    }

    std::size_t selected() const { return selected_; }

    CellSize wanted(int windowColumns, int windowRows) const override {
        CellSize s;
        s.columns = std::min(windowColumns, std::clamp(windowColumns - 4, 24, 64));
        const int lines = int(dialog_detail::wrap(question_, std::max(s.columns - 4, 1)).size());
        s.rows = std::min(windowRows, lines + 4);
        return s;
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
    }

    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 8 || rows_ < 4) return;
        using namespace dialog_detail;
        frame(cells, columns_, rows_);
        const auto lines = wrap(question_, columns_ - 4);
        for (std::size_t i = 0; i < lines.size() && int(i) < rows_ - 3; ++i) {
            cells.draw(2, 1 + int(i), lines[i], kFg, kBackground);
        }
        const auto spans = buttonSpans();
        for (std::size_t i = 0; i < buttons_.size(); ++i) {
            const bool on = i == selected_;
            std::u32string label = U"[ ";
            for (const char c : buttons_[i].label) label += char32_t(static_cast<unsigned char>(c));
            label += U" ]";
            cells.draw(spans[i].first, rows_ - 2, label, on ? kBackground : kFg, on ? kAccent : kBackground);
        }
    }

    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl || chord.alt) return out;
        switch (chord.key) {
        case keys::Escape:
            return choose(cancel_);
        case keys::Enter:
            return choose(selected_);
        case keys::Left:
            if (selected_ > 0) --selected_;
            return out;
        case keys::Right:
        case keys::Tab:
            if (selected_ + 1 < buttons_.size()) ++selected_;
            else if (chord.key == keys::Tab) selected_ = 0;
            return out;
        default:
            return out;
        }
    }

    LayerOutcome click(int column, int row) override {
        LayerOutcome out;
        if (row != rows_ - 2) return out;
        const auto spans = buttonSpans();
        for (std::size_t i = 0; i < spans.size(); ++i) {
            if (column >= spans[i].first && column < spans[i].second) return choose(i);
        }
        return out;
    }
    void wheel(double) override {}

private:
    LayerOutcome choose(std::size_t index) const {
        LayerOutcome out;
        out.close = true;
        if (index < buttons_.size()) out.action = buttons_[index].action;
        return out;
    }
    // [begin, end) cell range of each button, centred on the last row.
    std::vector<std::pair<int, int>> buttonSpans() const {
        int total = 0;
        for (const auto &b : buttons_) total += int(b.label.size()) + 4;
        total += buttons_.empty() ? 0 : int(buttons_.size() - 1) * 2;
        int x = std::max(1, (columns_ - total) / 2);
        std::vector<std::pair<int, int>> spans;
        for (const auto &b : buttons_) {
            const int width = int(b.label.size()) + 4;
            spans.emplace_back(x, x + width);
            x += width + 2;
        }
        return spans;
    }

    std::string question_;
    std::vector<Button> buttons_;
    std::size_t cancel_;
    std::size_t selected_;
    int columns_ = 0;
    int rows_ = 0;
};

class TextPrompt final : public Layer {
public:
    // Enter answers `actionPrefix + text`; with allowEmpty false an empty text does nothing.
    TextPrompt(std::string title, const std::string &initial, std::string actionPrefix, bool allowEmpty, std::size_t maxLength = 64)
        : title_(std::move(title))
        , prefix_(std::move(actionPrefix))
        , allowEmpty_(allowEmpty)
        , input_(maxLength) {
        for (const unsigned char c : initial) {
            if (c < 0x80) input_.insert(char32_t(c)); // Non-ASCII initial text is dropped, not mangled.
        }
    }

    const std::string &text() const { return text_; }
    std::string current() const { return input_.utf8(); }

    CellSize wanted(int windowColumns, int windowRows) const override {
        return {std::min(windowColumns, std::clamp(windowColumns - 4, 16, 48)), std::min(windowRows, 4)};
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
    }
    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 8 || rows_ < 4) return;
        using namespace dialog_detail;
        frame(cells, columns_, rows_);
        std::u32string title;
        for (const char c : title_) title += char32_t(static_cast<unsigned char>(c));
        cells.draw(2, 1, title.substr(0, std::size_t(columns_ - 4)), kDim, kBackground);
        const int fieldWidth = std::max(columns_ - 6, 1);
        const auto first = input_.firstVisible(std::size_t(fieldWidth));
        cells.draw(2, 2, U"> ", kDim, kBackground);
        const auto &text = input_.text();
        for (int i = 0; i < fieldWidth && first + std::size_t(i) < text.size(); ++i) {
            cells.at(static_cast<std::size_t>(4) + static_cast<std::size_t>(i), 2) = FChar{text[first + static_cast<std::size_t>(i)], kFg, kBackground};
        }
        cursor.visible = true;
        cursor.column = 4 + int(input_.cursor() - first);
        cursor.row = 2;
        cursor.shape = CursorState::Shape::Bar;
        cursor.blink = false;
        cursor.color = kFg;
    }
    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl) {
            if (foldKey(chord.key) == 'u') input_.clear();
            return out;
        }
        switch (chord.key) {
        case keys::Escape:
            out.close = true;
            return out;
        case keys::Enter:
            if (input_.empty() && !allowEmpty_) return out;
            out.close = true;
            out.action = prefix_ + input_.utf8();
            return out;
        case keys::Left: input_.left(); return out;
        case keys::Right: input_.right(); return out;
        case keys::Backspace: input_.backspace(); return out;
        default: break;
        }
        if (!chord.alt && chord.key >= 32) input_.insert(char32_t(chord.key));
        return out;
    }
    LayerOutcome click(int, int) override { return {}; }
    void wheel(double) override {}

private:
    std::string title_, prefix_;
    bool allowEmpty_;
    TextInput input_;
    std::string text_;
    int columns_ = 0;
    int rows_ = 0;
};

class PickList final : public Layer {
public:
    struct Item {
        std::string label;
        std::string detail; // Shown dim at the right.
        std::string action; // LayerOutcome::action when chosen
    };
    PickList(std::string title, std::vector<Item> items, std::string emptyText = "Nothing to choose")
        : title_(std::move(title))
        , items_(std::move(items))
        , empty_(std::move(emptyText)) {
        list_.setCount(items_.size());
    }

    std::size_t selected() const { return list_.selected(); }

    CellSize wanted(int windowColumns, int windowRows) const override {
        const int rowsWanted = int(std::clamp<std::size_t>(items_.size(), 1, 10)) + 3;
        return {std::min(windowColumns, std::clamp(windowColumns - 2, 12, 64)), std::min(windowRows, rowsWanted)};
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
        list_.setVisible(std::size_t(std::max(rows_ - 3, 1)));
    }
    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 8 || rows_ < 4) return;
        using namespace dialog_detail;
        frame(cells, columns_, rows_);
        const auto asText = [](const std::string &s) {
            std::u32string t;
            for (const char c : s) {
                const auto u = static_cast<unsigned char>(c);
                t += (u < 0x20 || u >= 0x7f) ? U'?' : char32_t(u);
            }
            return t;
        };
        cells.draw(2, 1, asText(title_).substr(0, std::size_t(columns_ - 4)), kDim, kBackground);
        if (items_.empty()) {
            cells.draw(2, 2, asText(empty_).substr(0, std::size_t(columns_ - 4)), kDim, kBackground);
            return;
        }
        for (int row = 0; row < rows_ - 3; ++row) {
            const std::size_t index = list_.first() + std::size_t(row);
            if (index >= items_.size()) break;
            const Rgb8 bg = index == list_.selected() ? kSelected : kBackground;
            for (int x = 1; x < columns_ - 1; ++x) cells.at(static_cast<std::size_t>(x), static_cast<std::size_t>(2) + static_cast<std::size_t>(row)) = FChar{U' ', kFg, bg};
            const auto detail = asText(items_[index].detail);
            const int detailWidth = int(detail.size());
            const auto label = asText(items_[index].label);
            const bool showDetail = detailWidth > 0 && int(label.size()) + 3 + detailWidth <= columns_ - 4;
            cells.draw(2, 2 + row, label.substr(0, std::size_t(columns_ - 4)), kFg, bg);
            if (showDetail) cells.draw(columns_ - 2 - detailWidth, 2 + row, detail, kDim, bg);
        }
    }
    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl) {
            const int k = foldKey(chord.key);
            if (k == 'n') list_.down();
            else if (k == 'p') list_.up();
            return out;
        }
        switch (chord.key) {
        case keys::Escape: out.close = true; return out;
        case keys::Enter: return choose(list_.selected());
        case keys::Up: list_.up(); return out;
        case keys::Down: list_.down(); return out;
        case keys::PageUp: list_.pageUp(); return out;
        case keys::PageDown: list_.pageDown(); return out;
        default: return out;
        }
    }
    LayerOutcome click(int column, int row) override {
        if (column < 1 || column >= columns_ - 1 || row < 2 || row >= rows_ - 1) return {};
        const std::size_t index = list_.first() + std::size_t(row - 2);
        if (index >= items_.size()) return {};
        list_.select(index);
        return choose(index);
    }
    void wheel(double amount) override {
        if (amount != 0) list_.scroll(amount > 0 ? -3 : 3);
    }

private:
    LayerOutcome choose(std::size_t index) const {
        LayerOutcome out;
        if (index >= items_.size()) return out;
        out.close = true;
        out.action = items_[index].action;
        return out;
    }
    std::string title_;
    std::vector<Item> items_;
    std::string empty_;
    ListView list_;
    int columns_ = 0;
    int rows_ = 0;
};

} // namespace frontend::ui
