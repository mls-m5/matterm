#pragma once

// The command palette (docs/TABS_PLAN.md section 8): a text field over a list of
// commands filtered by searchCommands(), each with its key. Enter or a click on an
// entry runs it, Escape closes. A Layer: no drawing backend, no window.
#include "commandregistry.h"
#include "layer.h"
#include "listview.h"
#include "textinput.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace frontend::ui {

class CommandPalette final : public Layer {
public:
    // `preferDesktop`: show the desktop shortcut beside an entry, else the prefix keys
    // (and the other kind when an entry has only that).
    explicit CommandPalette(std::vector<CommandEntry> entries, bool preferDesktop = false)
        : entries_(std::move(entries))
        , preferDesktop_(preferDesktop) {
        refilter();
    }

    const std::string &query() const {
        return queryText_;
    }
    std::size_t resultCount() const {
        return results_.size();
    }
    const CommandEntry *selectedEntry() const {
        return results_.empty() ? nullptr : results_[list_.selected()].entry;
    }
    std::size_t firstVisible() const {
        return list_.first();
    }

    CellSize wanted(int windowColumns, int windowRows) const override {
        CellSize s;
        s.columns = std::min(windowColumns, std::clamp(windowColumns - 2, 12, 64));
        const int listRows = std::clamp(windowRows - 6, 1, 10);
        s.rows = std::min(windowRows, listRows + 3);
        return s;
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
        list_.setVisible(std::size_t(std::max(rows_ - 3, 1)));
    }

    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 4 || rows_ < 3) {
            return;
        }
        const FChar border{U' ', kFg, kBorder};
        const FChar inner{U' ', kFg, kBackground};
        cells.clear(border);
        for (int y = 1; y < rows_ - 1; ++y) {
            for (int x = 1; x < columns_ - 1; ++x) {
                cells.at(std::size_t(x), std::size_t(y)) = inner;
            }
        }
        const int width = columns_ - 2; // inner width
        // The input line: "> text".
        put(cells, 1, 1, U"> ", kDim, kBackground);
        const int fieldWidth = std::max(width - 2, 1);
        const auto &text = input_.text();
        const auto first = input_.firstVisible(std::size_t(fieldWidth));
        for (int i = 0; i < fieldWidth && first + std::size_t(i) < text.size(); ++i) {
            cells.at(std::size_t(3 + i), 1) =
                FChar{text[first + std::size_t(i)], kFg, kBackground};
        }
        cursor.visible = true;
        cursor.column = 3 + int(input_.cursor() - first);
        cursor.row = 1;
        cursor.shape = CursorState::Shape::Bar;
        cursor.blink = false; // A steady bar: typing is what changes the frame.
        cursor.color = kFg;
        // The list.
        const int listRows = rows_ - 3;
        if (results_.empty()) {
            put(cells, 2, 2, U"No matching command", kDim, kBackground);
            return;
        }
        for (int row = 0; row < listRows; ++row) {
            const std::size_t index = list_.first() + std::size_t(row);
            if (index >= results_.size()) {
                break;
            }
            const bool selected = index == list_.selected();
            const Rgb8 bg = selected ? kSelected : kBackground;
            for (int x = 1; x < columns_ - 1; ++x) {
                cells.at(std::size_t(x), std::size_t(2 + row)) = FChar{U' ', kFg, bg};
            }
            const auto &r = results_[index];
            const auto keysText = keyString(*r.entry);
            const int keyWidth = int(keysText.size());
            // The keys win over the end of a long name: a name is cut, the keys are not.
            const bool showKeys = keyWidth > 0 && keyWidth + 8 <= width;
            const int nameLimit = showKeys ? std::min(int(r.entry->name.size()), width - keyWidth - 3)
                                           : int(r.entry->name.size());
            std::size_t offset = 0;
            for (std::size_t i = 0; int(i) < nameLimit && 2 + int(i) < columns_ - 1; ++i) {
                const bool hit = offset < r.match.offsets.size() && r.match.offsets[offset] == i;
                if (hit) {
                    ++offset;
                }
                FChar c{char32_t(static_cast<unsigned char>(r.entry->name[i])),
                        hit ? kAccent : kFg,
                        bg};
                if (hit) {
                    c.attributes = CellAttribute::Bold;
                }
                cells.at(std::size_t(2 + int(i)), std::size_t(2 + row)) = c;
            }
            if (showKeys) {
                const int at = columns_ - 2 - keyWidth;
                for (int i = 0; i < keyWidth; ++i) {
                    cells.at(std::size_t(at + i), std::size_t(2 + row)) =
                        FChar{char32_t(static_cast<unsigned char>(keysText[std::size_t(i)])), kDim, bg};
                }
            }
        }
    }

    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl) {
            switch (foldKey(chord.key)) {
            case 'n':
                list_.down();
                break;
            case 'p':
                list_.up();
                break;
            case 'u':
                if (input_.clear()) {
                    refilter();
                }
                break;
            default:
                break;
            }
            return out;
        }
        switch (chord.key) {
        case keys::Escape:
            out.close = true;
            return out;
        case keys::Enter:
            return run(list_.selected());
        case keys::Up:
            list_.up();
            return out;
        case keys::Down:
            list_.down();
            return out;
        case keys::PageUp:
            list_.pageUp();
            return out;
        case keys::PageDown:
            list_.pageDown();
            return out;
        case keys::Left:
            input_.left();
            return out;
        case keys::Right:
            input_.right();
            return out;
        case keys::Backspace:
            if (input_.backspace()) {
                refilter();
            }
            return out;
        case keys::Tab:
            return out;
        default:
            break;
        }
        if (!chord.alt && chord.key >= 32 && input_.insert(char32_t(chord.key))) {
            refilter();
        }
        return out;
    }

    LayerOutcome click(int column, int row) override {
        LayerOutcome out;
        if (column < 1 || column >= columns_ - 1 || row < 2 || row >= rows_ - 1) {
            return out;
        }
        const std::size_t index = list_.first() + std::size_t(row - 2);
        if (index >= results_.size()) {
            return out;
        }
        list_.select(index);
        return run(index);
    }

    void wheel(double amount) override {
        if (amount != 0) {
            list_.scroll(amount > 0 ? -3 : 3);
        }
    }

private:
    static constexpr Rgb8 kBorder{62, 84, 128};
    static constexpr Rgb8 kBackground{28, 32, 40};
    static constexpr Rgb8 kSelected{52, 66, 98};
    static constexpr Rgb8 kFg{230, 230, 230};
    static constexpr Rgb8 kDim{170, 178, 190};
    static constexpr Rgb8 kAccent{120, 190, 255};

    static void put(CellBuffer &cells, int x, int y, std::u32string_view text, Rgb8 fg, Rgb8 bg) {
        cells.draw(x, y, text, fg, bg);
    }

    std::string keyString(const CommandEntry &e) const {
        const auto &first = preferDesktop_ ? e.desktopKeys : e.prefixKeys;
        const auto &second = preferDesktop_ ? e.prefixKeys : e.desktopKeys;
        if (!first.empty()) return first.front();
        return second.empty() ? std::string() : second.front();
    }

    LayerOutcome run(std::size_t index) {
        LayerOutcome out;
        if (index >= results_.size()) {
            return out;
        }
        const CommandEntry &e = *results_[index].entry;
        out.close = true;
        if (e.call) {
            out.call = e.call;
        }
        else {
            out.action = e.id;
        }
        return out;
    }

    void refilter() {
        queryText_ = input_.utf8();
        results_ = searchCommands(entries_, queryText_);
        list_.setCount(results_.size());
    }

    std::vector<CommandEntry> entries_;
    bool preferDesktop_;
    TextInput input_;
    std::string queryText_;
    std::vector<RankedEntry> results_;
    ListView list_;
    int columns_ = 0;
    int rows_ = 0;
};

} // namespace frontend::ui
