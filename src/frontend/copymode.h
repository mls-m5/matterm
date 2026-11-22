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
        const int k = foldKey(chord.key);
        const bool plain = !chord.ctrl && !chord.alt;
        const std::size_t lastRow = lastRowOf(model);
        const std::size_t page = std::max<std::size_t>(model.rows(), 1);
        bool moved = true;
        if (chord.ctrl && !chord.alt) {
            switch (k) {
            case 'c':
                return leave(selection);
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
                selecting_ = false;
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
            if (selecting_) {
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

    // The badge for the pane's corner: "COPY 12/340" or "SELECT 12/340" (line
    // of all lines).
    std::string badge(const TerminalModel &model) const {
        const auto first = model.firstRetainedRow();
        const auto total = model.historySize() + model.rows();
        const auto line = std::min(row_ - std::min(row_, first) + 1, total);
        return std::string(selecting_ ? " SELECT " : " COPY ") +
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
    }
    Result leave(std::optional<Selection> &selection) {
        selection.reset();
        selecting_ = false;
        Result r;
        r.changed = true;
        r.action = Action::Leave;
        return r;
    }
    Result copy(const TerminalModel &model,
                std::optional<Selection> &selection) {
        Result r;
        r.changed = true;
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
};

} // namespace frontend
