#pragma once

// Copy / scroll mode (docs/TABS_PLAN.md section 12): a virtual cursor that
// moves through the retained text with the keyboard, selects and copies,
// without sending anything to the program. No SDL: the front end turns its key
// events into KeyChord and draws apply() and badge().
#include "prefixkeys.h"
#include "textselection.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace frontend {

class CopyMode {
public:
    using Cell = TerminalModel::TextCell;
    enum class Action {
        None,
        Leave, // leave the mode, nothing to copy
        Copy,  // copy `text`, then leave
    };
    struct Result {
        bool changed = false; // redraw
        Action action = Action::None;
        std::string text;      // for Copy
        bool tooLarge = false; // Copy: the selection exceeds the copy limit
                               // (nothing copied)
    };

    // Starts on the real cursor when it is on screen, else in the middle of
    // what is shown.
    explicit CopyMode(const TerminalModel &model) {
        const auto first = model.firstRetainedRow();
        if (model.viewport().atTail()) {
            const auto c = model.cursor();
            row_ =
                first + model.historySize() + std::size_t(std::max(c.row, 0));
            column_ = std::size_t(std::max(c.column, 0));
        }
        else {
            row_ = first + model.viewport().firstRow() + model.rows() / 2;
            column_ = 0;
        }
        clamp(model);
    }

    Cell cursor() const {
        return {row_, column_};
    }
    bool selecting() const {
        return selecting_;
    }

    Result key(const KeyChord &chord,
               TerminalModel &model,
               std::optional<Selection> &selection) {
        Result result;
        clamp(model);
        message_.clear();
        if (searching_) {
            return searchKey(chord, model, selection);
        }
        if (rect_ && rectEpoch_ != model.selectionEpoch()) {
            rect_ = selecting_ =
                false; // The text moved under it (resize, reset).
        }
        const int k = foldKey(chord.key);
        const bool plain = !chord.ctrl && !chord.alt;
        const std::size_t lastRow = lastRowOf(model);
        const std::size_t page = std::max<std::size_t>(model.rows(), 1);
        bool moved = true;
        if (chord.ctrl && !chord.alt) {
            switch (k) {
            case 'c':
                return leave(selection);
            case 'v':
                // Rectangular selection: the block between the anchor and the
                // cursor.
                if (selecting_ && rect_) {
                    selecting_ = rect_ = false;
                }
                else {
                    selection.reset();
                    selecting_ = rect_ = true;
                    rectRow_ = row_;
                    rectColumn_ = column_;
                    rectEpoch_ = model.selectionEpoch();
                }
                result.changed = true;
                moved = false;
                break;
            case 'b':
                moveUp(page);
                break;
            case 'f':
                moveDown(page, lastRow);
                break;
            case 'u':
                moveUp(std::max<std::size_t>(page / 2, 1));
                break;
            case 'd':
                moveDown(std::max<std::size_t>(page / 2, 1), lastRow);
                break;
            case keys::Up:
                moveUp(1);
                break;
            case keys::Down:
                moveDown(1, lastRow);
                break;
            case keys::Home:
                top(model);
                break;
            case keys::End:
                bottom(lastRow);
                break;
            default:
                moved = false;
                break;
            }
        }
        else if (plain && k == keys::Escape) {
            return leave(selection);
        }
        else if (plain && chord.key == 'q') {
            return leave(selection);
        }
        else if (plain && (k == keys::Enter || chord.key == 'y')) {
            return copy(model, selection);
        }
        else if (plain && (k == keys::Left || chord.key == 'h')) {
            if (column_ > 0) {
                --column_;
            }
        }
        else if (plain && (k == keys::Right || chord.key == 'l')) {
            ++column_;
        }
        else if (plain && (k == keys::Up || chord.key == 'k')) {
            moveUp(1);
        }
        else if (plain && (k == keys::Down || chord.key == 'j')) {
            moveDown(1, lastRow);
        }
        else if (plain && k == keys::PageUp) {
            moveUp(page);
        }
        else if (plain && k == keys::PageDown) {
            moveDown(page, lastRow);
        }
        else if (plain && (chord.key == keys::Home || chord.key == '0')) {
            column_ = 0;
        }
        else if (plain && chord.key == '^') {
            column_ = firstNonBlank(model, row_);
        }
        else if (plain && (chord.key == keys::End || chord.key == '$')) {
            column_ = lastNonBlank(model, row_);
        }
        else if (plain && chord.key == 'g') {
            top(model);
        }
        else if (plain && chord.key == 'G') {
            bottom(lastRow);
        }
        else if (plain && chord.key == 'w') {
            wordForward(model, lastRow);
        }
        else if (plain && chord.key == 'b') {
            wordBack(model);
        }
        else if (plain && chord.key == 'e') {
            wordEnd(model, lastRow);
        }
        else if (plain && chord.key == 'v') {
            if (selecting_) {
                selection.reset();
                selecting_ = rect_ = false;
            }
            else {
                begin(model, selection, 1);
            }
            result.changed = true;
            moved = false;
        }
        else if (plain && chord.key == ' ') {
            if (!selecting_) {
                begin(model, selection, 1);
            }
            result.changed = true;
            moved = false;
        }
        else if (plain && (chord.key == '/' || chord.key == '?')) {
            searching_ = true;
            searchForward_ = chord.key == '/';
            query_.clear();
            result.changed = true;
            moved = false;
        }
        else if (plain && (chord.key == 'n' || chord.key == 'N')) {
            if (lastQuery_.empty()) {
                message_ = "no previous search";
                moved = false;
                result.changed = true;
            }
            else {
                moved = search(model,
                               lastQuery_,
                               chord.key == 'n' ? lastForward_ : !lastForward_);
                result.changed = true;
            }
        }
        else if (plain && chord.key == 'V') {
            begin(model, selection, 3);
            result.changed = true;
            moved = false;
        }
        else {
            moved = false; // Consumed, but nothing to do: the program never
                           // sees it.
        }
        if (moved) {
            clamp(model);
            if (selecting_ && !rect_) {
                if (selection && selection->valid(model)) {
                    selection->extend(model, cursor());
                }
                else {
                    selecting_ = false;
                    selection.reset();
                }
            }
            result.changed = true;
        }
        ensureVisible(model);
        return result;
    }

    // Draws the virtual cursor over the frame (the real one is hidden).
    void apply(const TerminalModel &model, RenderFrame &frame) const {
        applyHighlights(model, frame);
        const auto viewFirst =
            model.firstRetainedRow() + model.viewport().firstRow();
        const bool shown = row_ >= viewFirst &&
                           row_ - viewFirst < frame.cells.rows() &&
                           column_ < frame.cells.columns();
        frame.cursor.visible = shown;
        if (!shown) {
            return;
        }
        frame.cursor.column = int(column_);
        frame.cursor.row = int(row_ - viewFirst);
        frame.cursor.shape = CursorState::Shape::Block;
        frame.cursor.blink = false;
        frame.cursor.focused = true;
        frame.cursor.columnOffset = 0;
        frame.cursor.rowOffset = 0;
    }

    // The block selection and the search matches, as extra intervals of the
    // frame.
    void applyHighlights(const TerminalModel &model, RenderFrame &frame) const {
        const auto viewFirst =
            model.firstRetainedRow() + model.viewport().firstRow();
        const auto columns = frame.cells.columns();
        const auto rows = frame.cells.rows();
        if (selecting_ && rect_) {
            const auto r1 = std::min(rectRow_, row_),
                       r2 = std::max(rectRow_, row_);
            const auto c1 = std::min(rectColumn_, column_),
                       c2 = std::max(rectColumn_, column_);
            for (std::size_t r = std::max(r1, viewFirst);
                 r <= r2 && r - viewFirst < rows;
                 ++r) {
                const auto base = (r - viewFirst) * columns;
                frame.selectionRanges.emplace_back(
                    base + c1, base + std::min(c2 + 1, columns));
            }
        }
        const std::string &pattern = searching_ ? query_ : lastQuery_;
        if (!pattern.empty()) {
            for (std::size_t y = 0; y < rows; ++y) {
                const auto text = rowText(model, viewFirst + y);
                for (const auto column : matchesIn(text, pattern)) {
                    const auto at = y * columns + column;
                    frame.matchRanges.emplace_back(at, at + pattern.size());
                }
            }
        }
    }

    // The badge for the pane's corner: "COPY 12/340" or "SELECT 12/340" (line
    // of all lines).
    std::string badge(const TerminalModel &model) const {
        const auto first = model.firstRetainedRow();
        const auto total = model.historySize() + model.rows();
        const auto line = std::min(row_ - std::min(row_, first) + 1, total);
        if (searching_) {
            return std::string(" ") + (searchForward_ ? "/" : "?") + query_ +
                   "_ ";
        }
        if (!message_.empty()) {
            return " " + message_ + " ";
        }
        return std::string(selecting_ ? (rect_ ? " BLOCK " : " SELECT ")
                                      : " COPY ") +
               std::to_string(line) + "/" + std::to_string(total) + " ";
    }

private:
    static std::size_t lastRowOf(const TerminalModel &model) {
        return model.firstRetainedRow() + model.historySize() + model.rows() -
               1;
    }
    void clamp(const TerminalModel &model) {
        const auto first = model.firstRetainedRow();
        row_ = std::clamp(row_, first, lastRowOf(model));
        column_ = std::min(column_, model.columns() - 1);
    }
    void moveUp(std::size_t n) {
        row_ = row_ > n ? row_ - n : 0; // clamp() raises it to the oldest row
    }
    void moveDown(std::size_t n, std::size_t lastRow) {
        row_ = std::min(row_ + n, lastRow);
    }
    void top(const TerminalModel &model) {
        row_ = model.firstRetainedRow();
        column_ = 0;
    }
    void bottom(std::size_t lastRow) {
        row_ = lastRow;
        column_ = 0;
    }

    static char32_t at(const TerminalModel &model,
                       std::size_t row,
                       std::size_t column) {
        const auto cp = model.textCodePoint({row, column});
        return cp ? *cp : U' ';
    }
    // 0 blank, 1 word character, 2 other.
    static int kind(char32_t cp) {
        if (cp == U' ' || cp == 0 || cp == 0xa0) {
            return 0;
        }
        const bool word = (cp >= U'a' && cp <= U'z') ||
                          (cp >= U'A' && cp <= U'Z') ||
                          (cp >= U'0' && cp <= U'9') || cp == U'_' || cp >= 128;
        return word ? 1 : 2;
    }
    static std::size_t firstNonBlank(const TerminalModel &model,
                                     std::size_t row) {
        for (std::size_t c = 0; c < model.columns(); ++c) {
            if (kind(at(model, row, c)) != 0) {
                return c;
            }
        }
        return 0;
    }
    static std::size_t lastNonBlank(const TerminalModel &model,
                                    std::size_t row) {
        for (std::size_t c = model.columns(); c-- > 0;) {
            if (kind(at(model, row, c)) != 0) {
                return c;
            }
        }
        return 0;
    }

    // Walks the text in reading order, a row end counting as a blank.
    bool step(const TerminalModel &model,
              std::size_t lastRow,
              Cell &cell,
              int direction) const {
        if (direction > 0) {
            if (cell.column + 1 < model.columns()) {
                ++cell.column;
                return true;
            }
            if (cell.row < lastRow) {
                ++cell.row;
                cell.column = 0;
                return true;
            }
            return false;
        }
        if (cell.column > 0) {
            --cell.column;
            return true;
        }
        if (cell.row > model.firstRetainedRow()) {
            --cell.row;
            cell.column = model.columns() - 1;
            return true;
        }
        return false;
    }
    int kindAt(const TerminalModel &model, Cell cell) const {
        return kind(at(model, cell.row, cell.column));
    }
    void wordForward(const TerminalModel &model, std::size_t lastRow) {
        Cell cell = cursor();
        const int start = kindAt(model, cell);
        // Past the rest of this word, then past the blanks.
        while (start != 0 && kindAt(model, cell) == start) {
            if (!step(model, lastRow, cell, 1)) {
                row_ = cell.row;
                column_ = cell.column;
                return;
            }
        }
        while (kindAt(model, cell) == 0) {
            if (!step(model, lastRow, cell, 1)) {
                break;
            }
        }
        row_ = cell.row;
        column_ = cell.column;
    }
    void wordBack(const TerminalModel &model) {
        Cell cell = cursor();
        const auto lastRow = lastRowOf(model);
        if (!step(model, lastRow, cell, -1)) {
            return;
        }
        while (kindAt(model, cell) == 0) {
            if (!step(model, lastRow, cell, -1)) {
                row_ = cell.row;
                column_ = cell.column;
                return;
            }
        }
        const int word = kindAt(model, cell);
        for (;;) {
            Cell before = cell;
            if (!step(model, lastRow, before, -1) ||
                kindAt(model, before) != word) {
                break;
            }
            cell = before;
        }
        row_ = cell.row;
        column_ = cell.column;
    }
    void wordEnd(const TerminalModel &model, std::size_t lastRow) {
        Cell cell = cursor();
        if (!step(model, lastRow, cell, 1)) {
            return;
        }
        while (kindAt(model, cell) == 0) {
            if (!step(model, lastRow, cell, 1)) {
                row_ = cell.row;
                column_ = cell.column;
                return;
            }
        }
        const int word = kindAt(model, cell);
        for (;;) {
            Cell after = cell;
            if (!step(model, lastRow, after, 1) ||
                kindAt(model, after) != word) {
                break;
            }
            cell = after;
        }
        row_ = cell.row;
        column_ = cell.column;
    }

    void begin(const TerminalModel &model,
               std::optional<Selection> &selection,
               unsigned clicks) {
        selection = Selection::at(model, cursor(), clicks);
        selecting_ = true;
        rect_ = false;
    }
    // ---- search ----
    // The text of one row as code points (a missing cell is a blank).
    static std::u32string rowText(const TerminalModel &model, std::size_t row) {
        std::u32string text;
        text.reserve(model.columns());
        for (std::size_t c = 0; c < model.columns(); ++c) {
            text.push_back(at(model, row, c));
        }
        return text;
    }
    static char32_t lower(char32_t c) {
        return c >= U'A' && c <= U'Z' ? c - U'A' + U'a' : c;
    }
    // Smart case: a pattern without capitals ignores case.
    static bool caseless(const std::string &pattern) {
        return std::none_of(pattern.begin(), pattern.end(), [](char c) {
            return c >= 'A' && c <= 'Z';
        });
    }
    // Start columns of the matches of an ASCII pattern in a row (matches never
    // wrap to the next row).
    static std::vector<std::size_t> matchesIn(const std::u32string &text,
                                              const std::string &pattern) {
        std::vector<std::size_t> found;
        if (pattern.empty() || pattern.size() > text.size()) {
            return found;
        }
        const bool fold = caseless(pattern);
        for (std::size_t i = 0; i + pattern.size() <= text.size(); ++i) {
            bool same = true;
            for (std::size_t j = 0; j < pattern.size() && same; ++j) {
                const char32_t a = text[i + j],
                               b = char32_t(
                                   static_cast<unsigned char>(pattern[j]));
                same = fold ? lower(a) == lower(b) : a == b;
            }
            if (same) {
                found.push_back(i);
            }
        }
        return found;
    }
    // Moves to the next (or previous) match after the cursor, wrapping around
    // the end. False and a message when there is none.
    bool search(const TerminalModel &model,
                const std::string &pattern,
                bool forward) {
        lastQuery_ = pattern;
        lastForward_ = forward;
        const auto first = model.firstRetainedRow();
        const auto last = lastRowOf(model);
        const auto total = last - first + 1;
        // Rows in visiting order, starting with the cursor's own.
        for (std::size_t step = 0; step <= total; ++step) {
            const std::size_t offset =
                forward ? (row_ - first + step) % total
                        : (row_ - first + total - step % total) % total;
            const std::size_t row = first + offset;
            auto found = matchesIn(rowText(model, row), pattern);
            if (found.empty()) {
                continue;
            }
            std::optional<std::size_t> pick;
            if (forward) {
                for (const auto c : found) {
                    if (step > 0 || c > column_ ||
                        (step == total && c <= column_)) {
                        pick = c;
                        break;
                    }
                }
            }
            else {
                for (auto it = found.rbegin(); it != found.rend(); ++it) {
                    if (step > 0 || *it < column_) {
                        pick = *it;
                        break;
                    }
                }
            }
            if (pick) {
                if (step == total || (forward ? row < row_ : row > row_)) {
                    message_ = "search wrapped";
                }
                row_ = row;
                column_ = *pick;
                return true;
            }
        }
        message_ = "not found: " + pattern;
        return false;
    }
    Result searchKey(const KeyChord &chord,
                     TerminalModel &model,
                     std::optional<Selection> &selection) {
        Result r;
        r.changed = true;
        if ((chord.ctrl && foldKey(chord.key) == 'c') ||
            chord.key == keys::Escape) {
            searching_ = false;
            return r;
        }
        if (chord.key == keys::Enter) {
            searching_ = false;
            if (!query_.empty()) {
                search(model, query_, searchForward_);
            }
            else if (!lastQuery_.empty()) {
                search(model, lastQuery_, searchForward_);
            }
            clamp(model);
            if (selecting_ && !rect_ && selection && selection->valid(model)) {
                selection->extend(model, cursor());
            }
            ensureVisible(model);
            return r;
        }
        if (chord.key == keys::Backspace) {
            if (query_.empty()) {
                searching_ = false;
            }
            else {
                query_.pop_back();
            }
            return r;
        }
        if (!chord.ctrl && !chord.alt && chord.key >= 32 && chord.key < 127 &&
            query_.size() < 200) {
            query_.push_back(char(chord.key));
        }
        return r;
    }
    Result copyRect(const TerminalModel &model,
                    std::optional<Selection> &selection) {
        Result r;
        r.changed = true;
        selection.reset();
        selecting_ = rect_ = false;
        const auto r1 = std::min(rectRow_, row_), r2 = std::max(rectRow_, row_);
        const auto c1 = std::min(rectColumn_, column_),
                   c2 = std::max(rectColumn_, column_);
        std::string text;
        for (auto row = r1; row <= r2; ++row) {
            auto line =
                model.copyText({row, c1}, {row, c2}, Selection::copyLimit);
            if (!line ||
                text.size() + line->size() + 1 > Selection::copyLimit) {
                r.action = Action::Leave;
                r.tooLarge = true;
                return r;
            }
            while (!line->empty() && line->back() == ' ') {
                line->pop_back();
            }
            text += *line;
            if (row != r2) {
                text += '\n';
            }
        }
        r.action = Action::Copy;
        r.text = std::move(text);
        return r;
    }

    Result leave(std::optional<Selection> &selection) {
        selection.reset();
        selecting_ = rect_ = false;
        searching_ = false;
        Result r;
        r.changed = true;
        r.action = Action::Leave;
        return r;
    }
    Result copy(const TerminalModel &model,
                std::optional<Selection> &selection) {
        Result r;
        r.changed = true;
        if (rect_ && selecting_) {
            return copyRect(model, selection);
        }
        std::optional<Selection> source = selection;
        if (!source || !source->valid(model)) {
            source =
                Selection::at(model, cursor(), 3); // The line under the cursor.
        }
        const auto text = source->text(model);
        selection.reset();
        selecting_ = false;
        if (!text) {
            r.action = Action::Leave;
            r.tooLarge = true;
            return r;
        }
        r.action = Action::Copy;
        r.text = *text;
        return r;
    }
    void ensureVisible(TerminalModel &model) const {
        const auto &viewport = model.viewport();
        const double firstPosition =
            viewport.maxOffset() - viewport.targetOffset(); // top row shown
        const double cursorPosition = double(row_ - model.firstRetainedRow());
        const double rows = double(model.rows());
        if (cursorPosition < firstPosition) {
            model.scrollBy(firstPosition - cursorPosition);
        }
        else if (cursorPosition > firstPosition + rows - 1) {
            model.scrollBy(-(cursorPosition - (firstPosition + rows - 1)));
        }
    }

    std::size_t row_ = 0;
    std::size_t column_ = 0;
    bool selecting_ = false;
    bool rect_ = false; // The selection is a block (Ctrl+V), anchored at
                        // rectRow_/rectColumn_.
    std::size_t rectRow_ = 0, rectColumn_ = 0, rectEpoch_ = 0;
    bool searching_ = false; // Typing a search ("/" or "?").
    bool searchForward_ = true;
    std::string query_;     // Being typed.
    std::string lastQuery_; // The last search that ran (n / N repeat it,
                            // matches are marked).
    bool lastForward_ = true;
    std::string message_; // Shown in the badge until the next key.
};

} // namespace frontend
