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

#include <algorithm>
#include <cstddef>
#include <vector>

namespace reflow {

// A run of padding longer than this inside a line is taken as content (bounds the buffer).
constexpr std::size_t kMaxPendingPadding = 8192;

// A position in the input to follow through the layout. `row` is an input row and `column` a cell in
// it; `after` means the place right after that cell (the cursor after its last typed character, or
// the pending-wrap position). Results: the new row index among the emitted rows (0 = first emitted)
// and the column. With `keep`, padding in front of the position is kept (a cursor that sits behind
// the text of its line, e.g. after a prompt, must stay behind it). When `after` lands exactly on the
// start of a new row, the position is reported as pending: the last cell of the row before.
struct Anchor {
    std::size_t row = 0;
    std::size_t column = 0;
    bool after = false;
    bool keep = false;
    bool found = false;
    std::size_t outRow = 0;
    std::size_t outColumn = 0;
    bool outPending = false;
    std::size_t position = 0; // Cell index in its line (internal).
};

struct Result {
    std::size_t rows = 0; // Rows handed to the sink.
};

// rowAt(i) -> FStringView, wrapAt(i) -> bool for i in [first, last); sink(const FChar *, count, wraps)
// gets each new row (count <= width; the caller pads); isPadding(const FChar &) tells padding cells.
template <class RowAt, class WrapAt, class Sink, class IsPadding>
Result layout(std::size_t first,
              std::size_t last,
              std::size_t width,
              RowAt &&rowAt,
              WrapAt &&wrapAt,
              Sink &&sink,
              IsPadding &&isPadding,
              Anchor *anchors = nullptr,
              std::size_t anchorCount = 0) {
    Result result;
    std::vector<FChar> out;
    std::vector<FChar> padding;
    out.reserve(width);
    std::size_t lineStart = 0;   // Emitted-row index where the current logical line starts.
    std::size_t cellsInLine = 0; // Cells consumed in this line so far (padding included).
    std::vector<Anchor *> inLine; // Anchors with `keep` found in the current line.
    const auto push = [&](const FChar &cell) {
        if (out.size() == width) {
            sink(out.data(), width, true);
            ++result.rows;
            out.clear();
        }
        out.push_back(cell);
    };
    for (std::size_t i = first; i < last; ++i) {
        for (std::size_t k = 0; k < anchorCount; ++k) {
            Anchor &a = anchors[k];
            if (a.row != i || a.found) {
                continue;
            }
            a.found = true;
            const std::size_t p = cellsInLine + a.column + (a.after ? 1 : 0);
            std::size_t q = p;
            if (a.after && p % width == 0) {
                a.outRow = lineStart + p / width - 1;
                a.outColumn = width - 1;
                a.outPending = true;
                q = p - 1;
            }
            else {
                a.outRow = lineStart + p / width;
                a.outColumn = p % width;
                a.outPending = false;
            }
            a.position = q;
            if (a.keep) {
                inLine.push_back(&a);
            }
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
            if (!inLine.empty()) {
                // The padding rule, except for the cells in front of a followed position. A position
                // far behind the text (a cursor in an empty row) is pulled back to the last row of
                // the line, so such a position does not make rows.
                const std::size_t contentEnd = (result.rows - lineStart) * width + out.size();
                const std::size_t lastRowEnd = ((contentEnd == 0 ? 1 : contentEnd) - 1) / width * width + width - 1;
                std::size_t keepThrough = 0;
                for (Anchor *a : inLine) {
                    if (a->position > lastRowEnd) {
                        // Right behind a full last row is the pending-wrap position.
                        const bool behind = a->position == lastRowEnd + 1 && contentEnd == lastRowEnd + 1;
                        a->position = lastRowEnd;
                        a->outRow = lineStart + lastRowEnd / width;
                        a->outColumn = lastRowEnd % width;
                        a->outPending = behind;
                    }
                    keepThrough = std::max(keepThrough, a->position);
                }
                const std::size_t allowed =
                    keepThrough + 1 > contentEnd ? std::min(padding.size(), keepThrough + 1 - contentEnd) : 0;
                for (std::size_t k = 0; k < allowed; ++k) {
                    push(padding[k]);
                }
                inLine.clear();
            }
            padding.clear();
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
