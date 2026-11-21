#pragma once

// CPU-only local selection shared by native/browser clients. No SDL or locale.
#include "terminalmodel.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace frontend {
struct Selection {
    using Cell = TerminalModel::TextCell;
    enum class Unit { Character, Word, Line };
    static constexpr std::size_t copyLimit = 1024 * 1024;

    Cell anchor, end; // Inclusive, direction-preserving endpoints.
    std::size_t epoch;
    Unit unit = Unit::Character;
    // Initial word/line range, retained when a drag crosses its starting unit.
    std::optional<std::pair<Cell, Cell>> origin;

    static bool before(Cell a, Cell b) {
        return std::tie(a.row, a.column) < std::tie(b.row, b.column);
    }

    bool valid(const TerminalModel &model) const {
        const auto first = model.firstRetainedRow();
        const auto count = model.historySize() + model.rows();
        return epoch == model.selectionEpoch() && anchor.row >= first &&
               end.row >= first && anchor.row - first < count &&
               end.row - first < count && anchor.column < model.columns() &&
               end.column < model.columns();
    }

    static Selection all(const TerminalModel &model) {
        const auto first = model.firstRetainedRow();
        return {{first, 0},
                {first + model.historySize() + model.rows() - 1,
                 model.columns() - 1},
                model.selectionEpoch()};
    }

    // Shift+click is always cell-granular and continues from the anchor. After
    // a double or triple click the anchor is the far end of the clicked word or
    // line (as seen from `cell`), so that unit stays selected, like in editors.
    void shiftTo(Cell cell) {
        if (origin) {
            anchor =
                before(cell, origin->first) ? origin->second : origin->first;
        }
        end = cell;
        unit = Unit::Character;
        origin.reset();
    }

    void extend(const TerminalModel &model, Cell cell) {
        if (!valid(model) || !model.textCodePoint(cell)) {
            return;
        }
        if (unit == Unit::Character || !origin) {
            end = cell;
            return;
        }
        const auto range = bounds(model, cell, unit);
        if (before(cell, origin->first)) {
            anchor = origin->second;
            end = range.first;
        }
        else {
            anchor = origin->first;
            end = range.second;
        }
    }

    static Selection at(const TerminalModel &model,
                        Cell cell,
                        unsigned clicks) {
        const auto unit = clicks >= 3   ? Unit::Line
                          : clicks == 2 ? Unit::Word
                                        : Unit::Character;
        const auto range = bounds(model, cell, unit);
        Selection result{range.first, range.second, model.selectionEpoch()};
        result.unit = unit;
        if (unit != Unit::Character) {
            result.origin = range;
        }
        return result;
    }

    std::optional<std::string> text(const TerminalModel &model,
                                    std::size_t limit = copyLimit) const {
        if (!valid(model)) {
            return std::nullopt;
        }
        return model.copyText(anchor, end, limit);
    }

    void apply(const TerminalModel &model, RenderFrame &frame) const {
        if (!valid(model)) {
            return;
        }
        // A plain click selects one cell. It still copies, but is not drawn.
        if (unit == Unit::Character && anchor.row == end.row &&
            anchor.column == end.column) {
            return;
        }
        auto first = anchor, last = end;
        if (before(last, first)) {
            std::swap(first, last);
        }
        const auto viewFirst =
            model.firstRetainedRow() + model.viewport().firstRow();
        const auto viewEnd = viewFirst + frame.cells.rows();
        if (last.row < viewFirst || first.row >= viewEnd) {
            return;
        }
        const auto columns = frame.cells.columns();
        frame.selectionBegin =
            first.row < viewFirst
                ? 0
                : (first.row - viewFirst) * columns + first.column;
        frame.selectionEnd =
            last.row >= viewEnd
                ? frame.cells.rows() * columns
                : (last.row - viewFirst) * columns + last.column + 1;
    }

private:
    // ASCII alphanumerics and path/URL punctuation form words. Non-ASCII
    // non-whitespace/non-punctuation is conservatively word-like (including
    // emoji): no locale dependence or claim of Unicode word-break support.
    static int category(char32_t cp) {
        const bool space =
            cp == U' ' || (cp >= 9 && cp <= 13) || cp == 0x85 || cp == 0xa0 ||
            cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
            cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (space) {
            return 0;
        }
        const bool asciiWord =
            (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') ||
            (cp >= U'0' && cp <= U'9') ||
            (cp < 128 && std::string_view("_-./:@~%+=?&#").find(char(cp)) !=
                             std::string_view::npos);
        const bool unicodePunctuation =
            (cp >= 0x200b && cp <= 0x206f) || (cp >= 0x2e00 && cp <= 0x2e7f) ||
            (cp >= 0x3001 && cp <= 0x303f) || (cp >= 0xff01 && cp <= 0xff0f) ||
            (cp >= 0xff1a && cp <= 0xff20) || (cp >= 0xff3b && cp <= 0xff40) ||
            (cp >= 0xff5b && cp <= 0xff65);
        return asciiWord || (cp >= 128 && !unicodePunctuation) ? 1 : 2;
    }

    static std::optional<Cell> previous(const TerminalModel &model, Cell cell) {
        if (cell.column > 0) {
            return Cell{cell.row, cell.column - 1};
        }
        if (cell.row > model.firstRetainedRow() &&
            model.textRowWrapped(cell.row - 1)) {
            return Cell{cell.row - 1, model.columns() - 1};
        }
        return std::nullopt;
    }
    static std::optional<Cell> next(const TerminalModel &model, Cell cell) {
        if (cell.column + 1 < model.columns()) {
            return Cell{cell.row, cell.column + 1};
        }
        if (model.textRowWrapped(cell.row) &&
            model.textCodePoint({cell.row + 1, 0})) {
            return Cell{cell.row + 1, 0};
        }
        return std::nullopt;
    }
    static std::pair<Cell, Cell> bounds(const TerminalModel &model,
                                        Cell cell,
                                        Unit unit) {
        Cell first = cell, last = cell;
        if (unit == Unit::Line) {
            while (first.row > model.firstRetainedRow() &&
                   model.textRowWrapped(first.row - 1)) {
                --first.row;
            }
            while (model.textRowWrapped(last.row) &&
                   model.textCodePoint({last.row + 1, 0})) {
                ++last.row;
            }
            first.column = 0;
            last.column = model.columns() - 1;
        }
        else if (unit == Unit::Word) {
            const auto cp = model.textCodePoint(cell);
            if (!cp) {
                return {first, last};
            }
            const auto kind = category(*cp);
            while (const auto prev = previous(model, first)) {
                if (category(*model.textCodePoint(*prev)) != kind) {
                    break;
                }
                first = *prev;
            }
            while (const auto following = next(model, last)) {
                if (category(*model.textCodePoint(*following)) != kind) {
                    break;
                }
                last = *following;
            }
        }
        return {first, last};
    }
};

// Shared click policy: a stale selection is never reused as a Shift anchor.
inline void selectAt(std::optional<Selection> &selection,
                     const TerminalModel &model,
                     Selection::Cell cell,
                     bool shift,
                     unsigned clicks) {
    if (shift && selection && selection->valid(model)) {
        selection->shiftTo(cell);
    }
    else {
        selection = Selection::at(model, cell, shift ? 1 : clicks);
    }
}
} // namespace frontend
