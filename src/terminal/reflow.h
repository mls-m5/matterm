#pragma once

// Laying saved rows out again at a new width (docs/SCROLLING_AND_REFLOW.md, "Reflow"). Pure
// algorithm over callbacks, no terminal state, so it is tested on its own and used by the
// model both for the immediate part of a width change and for the lazy slices.
//
// Rows are joined across soft wraps into logical lines. A hard line end (a row that does not wrap,
// or the last row given) ends the line. The trailing padding cells of a logical line are dropped
// ("padding rule": rows have no written extent, so default-looking blanks at the end of a line
// are taken to be padding, not text), the rest is cut into rows of `width` cells; every row of a
// line but its last is marked as wrapping. An empty line stays one empty row. Memory is bounded:
// one output row and a bounded run of pending padding, never a whole logical line.
#include "fchar.h"
#include "fstringview.h"

#include <cstddef>
#include <vector>

namespace reflow {

// A run of padding longer than this inside a line is taken as content (bounds the buffer).
constexpr std::size_t kMaxPendingPadding = 8192;

struct Result {
    std::size_t rows = 0;         // Rows handed to the sink.
    bool anchorFound = false;
    std::size_t anchorRow = 0;    // Index among the emitted rows (0 = first one emitted).
};

// rowAt(i) -> FStringView, wrapAt(i) -> bool for i in [first, last); sink(const FChar *, count, wraps)
// gets each new row (count <= width; the caller pads); isPadding(const FChar &) tells padding cells.
// `anchor`: an input row whose first cell's new row index is reported (rounded down to a row).
template <class RowAt, class WrapAt, class Sink, class IsPadding>
Result layout(std::size_t first,
              std::size_t last,
              std::size_t width,
              RowAt &&rowAt,
              WrapAt &&wrapAt,
              Sink &&sink,
              IsPadding &&isPadding,
              std::size_t anchor = static_cast<std::size_t>(-1)) {
    Result result;
    std::vector<FChar> out;
    std::vector<FChar> padding;
    out.reserve(width);
    std::size_t lineStart = 0;     // Emitted-row index where the current logical line starts.
    std::size_t cellsInLine = 0;   // Cells consumed in this line so far (padding included).
    const auto push = [&](const FChar &cell) {
        if (out.size() == width) {
            sink(out.data(), width, true);
            ++result.rows;
            out.clear();
        }
        out.push_back(cell);
    };
    for (std::size_t i = first; i < last; ++i) {
        if (i == anchor) {
            result.anchorFound = true;
            result.anchorRow = lineStart + cellsInLine / width;
        }
        const FStringView row = rowAt(i);
        for (std::size_t x = 0; x < row.size(); ++x) {
            const FChar &cell = row.data()[x];
            if (isPadding(cell)) {
                padding.push_back(cell);
                if (padding.size() > kMaxPendingPadding) {
                    for (const auto &p : padding) {
                        push(p);
                    }
                    padding.clear();
                }
            }
            else {
                for (const auto &p : padding) {
                    push(p);
                }
                padding.clear();
                push(cell);
            }
        }
        cellsInLine += row.size();
        if (i + 1 == last || !wrapAt(i)) {
            padding.clear(); // The padding rule.
            sink(out.data(), out.size(), false);
            ++result.rows;
            out.clear();
            lineStart = result.rows;
            cellsInLine = 0;
        }
    }
    return result;
}

} // namespace reflow
