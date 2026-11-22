#include "terminalmodel.h"

#include "reflow.h"
#include "terminalmemory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
void validateSize(std::size_t columns, std::size_t rows, std::size_t history) {
    const auto max = std::size_t(std::numeric_limits<int>::max());
    if (columns == 0 || rows == 0 || columns > max || rows > max ||
        history > std::numeric_limits<std::size_t>::max() - rows) {
        throw std::invalid_argument(
            "Invalid terminal dimensions or history capacity");
    }
}
} // namespace

struct TerminalModel::Impl {
    struct Screen {
        TerminalMemory memory;
        ScrollViewport viewport;
        std::deque<bool> softWrap;
        std::size_t firstRow = 0;

        void append(FStringView row, FChar fill, bool wraps = false) {
            softWrap.push_back(wraps);
            try {
                memory.append(row, fill);
            }
            catch (...) {
                softWrap.pop_back();
                throw;
            }
            if (softWrap.size() > memory.size()) {
                softWrap.pop_front();
                ++firstRow;
            }
        }
        // Adds an older row above the oldest one (lazy reflow). False when full.
        bool prepend(FStringView row, FChar fill, bool wraps) {
            if (firstRow == 0 || !memory.prepend(row, fill)) {
                return false;
            }
            softWrap.push_front(wraps);
            --firstRow;
            return true;
        }
        void popBack(std::size_t count) {
            count = std::min(count, softWrap.size());
            memory.popBack(count);
            softWrap.resize(softWrap.size() - count);
        }
        // Never-written cells use the default colors, like cleared ones, so a
        // theme change recolors both the same way.
        static FChar defaultCell() {
            FChar cell;
            cell.setForeground(ColorReference::defaultColor());
            cell.setBackground(ColorReference::defaultColor());
            return cell;
        }
        CursorState cursor;
        FChar style;
        bool wrapPending = false;
        int scrollTop = 0;
        int scrollBottom = 0;
        bool origin = false;
        bool autoWrap = true;
        bool insert = false;
        CursorState savedCursor;
        FChar savedStyle;
        bool savedWrap = false;
        bool hasSavedCursor = false;
        bool savedOrigin = false;
        bool savedAutoWrap = true;

        Screen(std::size_t columns,
               std::size_t rows,
               std::size_t history,
               FChar fill = defaultCell())
            : memory(columns, rows + history)
            , style(fill) {
            cursor.visible = true;
            cursor.shape = CursorState::Shape::Block;
            scrollBottom = int(rows) - 1;
            for (std::size_t i = 0; i < rows; ++i) {
                append(FStringView{}, fill);
            }
            viewport.setGeometry(memory.size(), rows);
        }
    };

    std::size_t columns;
    std::size_t rows;
    std::size_t scrollback;
    std::unique_ptr<Screen> primary;
    std::unique_ptr<Screen> alternate;
    // Lazy reflow (docs/SCROLLING_AND_REFLOW.md): after a width change the newest saved rows are
    // laid out at once; the older ones wait here, untouched and at their old width, and are
    // laid out in slices once the size has been quiet for a moment. The newest segment is last.
    struct Segment {
        std::unique_ptr<Screen> screen; // Its rows [0, count) are the ones still waiting.
        std::size_t count;
    };
    static constexpr std::size_t kImmediateRows = 1000;  // Saved rows laid out at once.
    static constexpr std::size_t kMinSegmentRows = 2000; // Not worth deferring less than this.
    static constexpr std::size_t kMaxSegments = 2;       // Each keeps its old screen memory alive.
    static constexpr std::size_t kSliceRows = 1000;
    static constexpr double kQuietSeconds = 0.1;
    std::vector<Segment> segments;
    double quiet = 0; // Seconds since the last resize.
    TerminalModel::LiveReflow liveMode = TerminalModel::LiveReflow::AboveCursor;
    bool useAlternate = false;
    std::size_t selectionEpoch = 0;
    std::vector<bool> tabStops;
    static std::vector<bool> defaultTabs(std::size_t width) {
        std::vector<bool> result(width, false);
        for (std::size_t x = 8; x < width; x += 8) {
            result[x] = true;
        }
        return result;
    }

    Impl(std::size_t width, std::size_t height, std::size_t history)
        : columns(width)
        , rows(height)
        , scrollback(history) {
        validateSize(width, height, history);
        primary = std::make_unique<Screen>(width, height, history);
        alternate = std::make_unique<Screen>(width, height, 0);
        tabStops = defaultTabs(width);
    }

    Screen &active() {
        return useAlternate ? *alternate : *primary;
    }
    const Screen &active() const {
        return useAlternate ? *alternate : *primary;
    }
    std::size_t start(const Screen &screen) const {
        return screen.memory.size() - rows;
    }
    FChar blank(const Screen &screen) const {
        auto result = screen.style;
        result.codePoint = U' ';
        result.setStyle(CellAttribute::None);
        return result;
    }

    void fillRow(Screen &screen,
                 std::size_t y,
                 std::size_t begin,
                 std::size_t end) {
        if (end == columns) {
            screen.softWrap[start(screen) + y] = false;
        }
        for (std::size_t x = begin; x < end; ++x) {
            screen.memory.at(x, start(screen) + y) = blank(screen);
        }
    }

    void copyRow(Screen &screen, int destination, int source) {
        auto *data =
            &screen.memory.at(0, start(screen) + std::size_t(destination));
        const auto row = screen.memory.row(start(screen) + std::size_t(source));
        std::memmove(data, row.data(), columns * sizeof(FChar));
        screen.softWrap[start(screen) + std::size_t(destination)] =
            screen.softWrap[start(screen) + std::size_t(source)];
    }
    void shiftRows(
        Screen &screen, int top, int bottom, int count, bool up, bool history) {
        if (count <= 0) {
            return;
        }
        count = std::min(count, bottom - top + 1);
        if (up && history && top == 0 && bottom == int(rows) - 1) {
            for (int i = 0; i < count; ++i) {
                screen.append(FStringView{}, blank(screen));
            }
            screen.viewport.rowsAppended(std::size_t(count),
                                         screen.memory.size());
            return;
        }
        ++selectionEpoch;
        if (top > 0) {
            screen.softWrap[start(screen) + std::size_t(top - 1)] = false;
        }
        if (up) {
            for (int y = top; y <= bottom - count; ++y) {
                copyRow(screen, y, y + count);
            }
            for (int y = bottom - count + 1; y <= bottom; ++y) {
                fillRow(screen, std::size_t(y), 0, columns);
            }
        }
        else {
            for (int y = bottom; y >= top + count; --y) {
                copyRow(screen, y, y - count);
            }
            for (int y = top; y < top + count; ++y) {
                fillRow(screen, std::size_t(y), 0, columns);
            }
        }
        if (bottom < int(rows) - 1) {
            screen.softWrap[start(screen) + std::size_t(bottom)] = false;
        }
    }

    // True when nothing visible is in the row: spaces with the screen's
    // erase background, or cells never written since the screen was created.
    bool blankRow(const Screen &screen, std::size_t y) const {
        const auto empty = blank(screen);
        const auto row = screen.memory.row(y);
        const auto sameBackground = [](const FChar &a, const FChar &b) {
            return a.backgroundColor().kind == b.backgroundColor().kind &&
                   a.background.r == b.background.r &&
                   a.background.g == b.background.g &&
                   a.background.b == b.background.b;
        };
        const FChar pristine;
        constexpr auto visibleStyle = CellAttribute::Underline |
                                      CellAttribute::Strike |
                                      CellAttribute::Inverse;
        for (std::size_t x = 0; x < columns; ++x) {
            const auto &cell = row.data()[x];
            if ((cell.codePoint != U' ' && cell.codePoint != 0) ||
                hasAttribute(cell.attributes, visibleStyle) ||
                !(sameBackground(cell, empty) ||
                  sameBackground(cell, pristine))) {
                return false;
            }
        }
        return true;
    }

    // The padding rule of reflow: a cell that looks the same as one never written (a space or NUL,
    // nothing drawn but the erase or the default background) at the end of a logical line is padding.
    bool paddingCell(const FChar &cell, const FChar &empty) const {
        constexpr auto visibleStyle = CellAttribute::Underline | CellAttribute::Strike | CellAttribute::Inverse;
        const auto sameBackground = [](const FChar &a, const FChar &b) {
            return a.backgroundColor().kind == b.backgroundColor().kind && a.background.r == b.background.r &&
                   a.background.g == b.background.g && a.background.b == b.background.b;
        };
        static const FChar pristine = Screen::defaultCell();
        return (cell.codePoint == U' ' || cell.codePoint == 0) && !hasAttribute(cell.attributes, visibleStyle) &&
               (sameBackground(cell, empty) || sameBackground(cell, pristine));
    }

    struct Resized {
        std::unique_ptr<Screen> screen;
        std::size_t segmentRows = 0; // Old rows [0, segmentRows) were left unreflowed (see segments).
    };

    // Where the older rows that wait (see segments) end: always at the start of a logical line.
    // Returns `firstOld` when nothing is deferred.
    std::size_t segmentCut(const Screen &old,
                           std::size_t firstOld,
                           std::size_t historyRows,
                           bool reading,
                           std::size_t readerRow) const {
        if (segments.size() >= kMaxSegments || historyRows <= kImmediateRows + kMinSegmentRows) {
            return firstOld;
        }
        std::size_t cut = firstOld + historyRows - kImmediateRows;
        if (reading) {
            cut = std::min(cut, readerRow); // The rows being read are laid out at once.
        }
        std::size_t steps = 0;
        while (cut > firstOld && old.softWrap[cut - 1] && steps++ < 100000) {
            --cut;
        }
        if ((cut > firstOld && old.softWrap[cut - 1]) || cut - firstOld < kMinSegmentRows) {
            return firstOld;
        }
        return cut;
    }

    Resized resized(const Screen &old,
                    std::size_t width,
                    std::size_t height,
                    std::size_t history,
                    bool discardHistory = false,
                    bool reflowHistory = false) const {
        if (reflowHistory && liveMode != TerminalModel::LiveReflow::Off && width != columns && !discardHistory) {
            return resizedLive(old, width, height, history);
        }
        auto result =
            std::make_unique<Screen>(width, height, history, blank(old));
        result->memory.clear();
        result->softWrap.clear();
        // Shrinking: rows that are blank and below the cursor go first, as in
        // xterm and VTE, so a program that printed a short frame at the top
        // keeps it on screen instead of having it pushed into the scrollback
        // (where its redraw after SIGWINCH would duplicate it).
        std::size_t dropped = 0;
        for (std::size_t limit = rows > height ? rows - height : 0;
             dropped < limit &&
             rows - 1 - dropped > std::size_t(old.cursor.row) &&
             blankRow(old, start(old) + rows - 1 - dropped);
             ++dropped) {
        }
        // Only primary saved lines are available to reveal on growth. Append
        // blank rows only after those saved lines have all become visible.
        const auto pulled = height > rows && history > 0 && !discardHistory
                                ? std::min(height - rows, start(old))
                                : 0;
        // Wrap flags describe how a line was written: they survive a width change on the primary
        // screen (reflowHistory), the alternate screen clips and forgets them as before.
        const bool keepFlags = width == columns || reflowHistory;
        const std::size_t firstOld = discardHistory ? start(old) : 0;
        const std::size_t endOld = old.memory.size() - dropped;
        const std::size_t extra = height > rows + pulled ? height - rows - pulled : 0;
        const std::size_t resultRows = endOld - firstOld + extra;
        const std::size_t historyRows = resultRows > height ? resultRows - height : 0;
        const bool reflowing = reflowHistory && width != columns && historyRows > 0 && !discardHistory;
        const bool reading = !old.viewport.atTail();
        const std::size_t readerRow = old.viewport.firstRow();
        std::size_t appended = 0; // Rows appended to the result, evicted ones included.
        std::size_t segmentRows = 0;
        reflow::Anchor readerAnchor;
        readerAnchor.row = readerRow;
        std::size_t liveFirst = firstOld; // First old row that is copied (clipped or padded) as it is.
        if (reflowing) {
            const std::size_t cut = segmentCut(old, firstOld, historyRows, reading, readerRow);
            segmentRows = cut - firstOld;
            const auto empty = blank(old);
            reflow::layout(
                cut,
                firstOld + historyRows,
                width,
                [&](std::size_t y) { return old.memory.row(y); },
                [&](std::size_t y) { return old.softWrap[y]; },
                [&](const FChar *cells, std::size_t count, bool wraps) {
                    result->append(FStringView(cells, count), empty, wraps);
                    ++appended;
                },
                [&](const FChar &cell) { return paddingCell(cell, empty); },
                &readerAnchor,
                reading ? 1 : 0);
            liveFirst = firstOld + historyRows;
        }
        for (std::size_t y = liveFirst; y < endOld; ++y) {
            result->append(old.memory.row(y), blank(old), keepFlags && old.softWrap[y]);
            ++appended;
        }
        for (std::size_t y = rows + pulled; y < height; ++y) {
            result->append(FStringView{}, blank(old));
            ++appended;
        }
        result->style = old.style;
        result->savedCursor = old.savedCursor;
        if (pulled > 0) {
            result->savedCursor.row = int(
                std::min(std::size_t(std::max(0, old.savedCursor.row)) + pulled,
                         height - 1));
        }
        result->savedStyle = old.savedStyle;
        result->savedWrap = old.savedWrap;
        result->hasSavedCursor = old.hasSavedCursor;
        result->savedOrigin = old.savedOrigin;
        result->savedAutoWrap = old.savedAutoWrap;
        result->autoWrap = old.autoWrap;
        result->insert = old.insert;
        result->cursor = old.cursor;
        result->cursor.column = std::min(old.cursor.column, int(width) - 1);
        const auto removed = rows > height ? rows - height - dropped : 0;
        const auto oldRow = std::size_t(old.cursor.row);
        result->cursor.row =
            int((oldRow >= removed ? oldRow - removed : 0) + pulled);
        result->cursor.row = std::min(result->cursor.row, int(height) - 1);
        result->wrapPending = old.wrapPending && oldRow >= removed;
        if (result->wrapPending && width > columns) {
            // Widening exposes the insertion position after the old last cell.
            result->cursor.column = old.cursor.column + 1;
            result->wrapPending = false;
        }
        if (discardHistory) {
            result->scrollTop = old.scrollTop;
            result->scrollBottom = old.scrollBottom;
            result->origin = old.origin;
        }
        result->viewport = old.viewport;
        result->viewport.rowsRevealed(pulled, result->memory.size(), height);
        if (reflowing && reading) {
            // Keep reading the same text: the first visible row is found again by its place in its line.
            const std::size_t evicted = appended - result->memory.size();
            const std::size_t liveStart = result->memory.size() - height;
            std::size_t first = 0;
            if (readerRow >= liveFirst) {
                first = liveStart + (readerRow - liveFirst);
            }
            else if (readerAnchor.found && readerAnchor.outRow >= evicted) {
                first = readerAnchor.outRow - evicted;
            }
            result->viewport.reanchor(first, result->memory.size(), height);
        }
        return {std::move(result), segmentRows};
    }

    // Width change on the primary screen with the live rows reflowed too: the saved rows and the screen
    // rows are one stream of lines. With LiveReflow::All the cursor line is laid out again as well (like
    // VTE); a shell that redraws its prompt on SIGWINCH assumes it was not, and leaves a copy of a wrapped
    // prompt behind, so by default the logical line the cursor is in and the rows below it are clipped and
    // padded as before and only what is above it is reflowed. Followed by their place in their line: the
    // cursor, the saved cursor and the reader's first row. Blank rows below the cursor are not content.
    Resized resizedLive(const Screen &old, std::size_t width, std::size_t height, std::size_t history) const {
        auto result = std::make_unique<Screen>(width, height, history, blank(old));
        result->memory.clear();
        result->softWrap.clear();
        const bool all = liveMode == TerminalModel::LiveReflow::All;
        const std::size_t oldHistory = start(old);
        const std::size_t cursorAbs = oldHistory + std::size_t(old.cursor.row);
        std::size_t end = old.memory.size();
        while (end > cursorAbs + 1 && blankRow(old, end - 1)) {
            --end;
        }
        const bool reading = !old.viewport.atTail();
        const std::size_t readerRow = old.viewport.firstRow();
        const std::size_t cut = segmentCut(old, 0, oldHistory, reading, readerRow);
        std::size_t split = end; // Rows [split, end) are clipped or padded as they are.
        if (!all) {
            split = cursorAbs;
            std::size_t steps = 0;
            while (split > cut && old.softWrap[split - 1] && steps++ < 100000) {
                --split;
            }
            if (split > cut && old.softWrap[split - 1]) {
                split = cut;
            }
        }
        const std::size_t savedAbs = oldHistory + std::size_t(std::max(0, old.savedCursor.row));
        const bool saved = old.hasSavedCursor && std::size_t(std::max(0, old.savedCursor.row)) < rows;
        reflow::Anchor anchors[3];
        std::size_t count = 0;
        std::size_t cursorIndex = 3;
        if (all) {
            anchors[count].row = cursorAbs;
            anchors[count].column = std::size_t(old.cursor.column);
            anchors[count].after = old.wrapPending;
            anchors[count].keep = true;
            cursorIndex = count++;
        }
        std::size_t readerIndex = 3;
        if (reading && readerRow >= cut && readerRow < split) {
            anchors[count].row = readerRow;
            readerIndex = count++;
        }
        std::size_t savedIndex = 3;
        if (saved && savedAbs < split) {
            anchors[count].row = savedAbs;
            anchors[count].column = std::size_t(std::max(0, old.savedCursor.column));
            anchors[count].keep = true;
            savedIndex = count++;
        }
        std::size_t appended = 0;
        const auto empty = blank(old);
        reflow::layout(
            cut,
            split,
            width,
            [&](std::size_t y) { return old.memory.row(y); },
            [&](std::size_t y) { return old.softWrap[y]; },
            [&](const FChar *cells, std::size_t n, bool wraps) {
                result->append(FStringView(cells, n), empty, wraps);
                ++appended;
            },
            [&](const FChar &cell) { return paddingCell(cell, empty); },
            anchors,
            count);
        const std::size_t splitOut = appended; // Emitted index of the first clipped row.
        for (std::size_t y = split; y < end; ++y) {
            result->append(old.memory.row(y), empty, old.softWrap[y]);
            ++appended;
        }
        const std::size_t evicted = appended - result->memory.size();
        const auto retained = [&](std::size_t index) { return index >= evicted ? index - evicted : 0; };
        // Where the cursor is, as an emitted row index.
        std::size_t cursorOut = splitOut + (cursorAbs - split);
        int cursorColumn = std::min(old.cursor.column, int(width) - 1);
        bool pending = old.wrapPending;
        if (all) {
            cursorOut = anchors[cursorIndex].outRow;
            cursorColumn = int(anchors[cursorIndex].outColumn);
            pending = anchors[cursorIndex].outPending;
        }
        else if (pending && width > columns) {
            cursorColumn = old.cursor.column + 1; // Widening exposes the position after the last cell.
            pending = false;
        }
        const std::size_t cursorRow = retained(cursorOut);
        std::size_t windowStart = 0;
        if (result->memory.size() > height) {
            windowStart = result->memory.size() - height;
            if (cursorRow < windowStart) {
                // More content below the cursor than the screen holds: the cursor stays on screen.
                windowStart = cursorRow;
                result->popBack(result->memory.size() - (cursorRow + height));
            }
        }
        while (result->memory.size() < windowStart + height) {
            result->append(FStringView{}, empty);
        }
        result->style = old.style;
        result->savedStyle = old.savedStyle;
        result->savedWrap = old.savedWrap && !all;
        result->hasSavedCursor = old.hasSavedCursor;
        result->savedOrigin = old.savedOrigin;
        result->savedAutoWrap = old.savedAutoWrap;
        result->autoWrap = old.autoWrap;
        result->insert = old.insert;
        result->cursor = old.cursor;
        result->cursor.row = int(std::min(cursorRow - windowStart, height - 1));
        result->cursor.column = cursorColumn;
        result->wrapPending = pending;
        result->savedCursor = old.savedCursor;
        const auto place = [&](std::size_t emitted) {
            const std::size_t row = retained(emitted);
            return int(row >= windowStart ? std::min(row - windowStart, height - 1) : 0);
        };
        if (saved && savedIndex < count && anchors[savedIndex].found) {
            result->savedCursor.row = place(anchors[savedIndex].outRow);
            result->savedCursor.column = int(anchors[savedIndex].outColumn);
        }
        else if (saved && savedAbs >= split && savedAbs < end) {
            result->savedCursor.row = place(splitOut + (savedAbs - split));
            result->savedCursor.column = std::min(old.savedCursor.column, int(width) - 1);
        }
        else {
            result->savedCursor.row = std::min(old.savedCursor.row, int(height) - 1);
            result->savedCursor.column = std::min(old.savedCursor.column, int(width) - 1);
        }
        result->viewport = old.viewport;
        result->viewport.setGeometry(result->memory.size(), height);
        if (reading) {
            std::size_t first = result->memory.size() - height;
            if (readerIndex < count && anchors[readerIndex].found) {
                first = std::min(retained(anchors[readerIndex].outRow), first);
            }
            else if (readerRow >= split && readerRow < end) {
                first = std::min(retained(splitOut + (readerRow - split)), first);
            }
            result->viewport.reanchor(first, result->memory.size(), height);
        }
        return {std::move(result), cut};
    }

    // Lays out the next slice of the waiting rows (newest first) above the saved rows.
    void reflowSlice(std::size_t maxRows) {
        if (segments.empty()) {
            return;
        }
        Screen &destination = *primary;
        if (destination.memory.size() >= destination.memory.capacity() || destination.firstRow == 0) {
            segments.clear(); // The ring is full: what is left would be evicted at once.
            return;
        }
        auto &segment = segments.back();
        const Screen &source = *segment.screen;
        const std::size_t hi = segment.count;
        std::size_t lo = hi > maxRows ? hi - maxRows : 0;
        std::size_t steps = 0;
        while (lo > 0 && source.softWrap[lo - 1] && steps++ < 100000) {
            --lo;
        }
        if (lo > 0 && source.softWrap[lo - 1]) {
            lo = 0;
        }
        const auto fill = blank(destination);
        std::vector<FChar> cells;
        std::vector<char> wraps;
        reflow::layout(
            lo,
            hi,
            columns,
            [&](std::size_t y) { return source.memory.row(y); },
            [&](std::size_t y) { return source.softWrap[y]; },
            [&](const FChar *row, std::size_t count, bool wrapped) {
                cells.insert(cells.end(), row, row + count);
                cells.insert(cells.end(), columns - count, fill);
                wraps.push_back(wrapped ? 1 : 0);
            },
            [&](const FChar &cell) { return paddingCell(cell, fill); });
        // Nothing below can throw: the rows are ready.
        const std::size_t room = destination.memory.capacity() - destination.memory.size();
        const std::size_t take = std::min(room, wraps.size());
        for (std::size_t k = wraps.size(); k > wraps.size() - take; --k) {
            if (!destination.prepend(FStringView(cells.data() + (k - 1) * columns, columns), fill, wraps[k - 1] != 0)) {
                segments.clear();
                break;
            }
        }
        destination.viewport.rowsPrepended(destination.memory.size());
        if (take < wraps.size()) {
            segments.clear(); // The oldest rows of this slice did not fit: nothing older will.
        }
        else if (lo == 0) {
            segments.pop_back();
        }
        else {
            segment.count = lo;
        }
    }
};

TerminalModel::TerminalModel(std::size_t columns,
                             std::size_t rows,
                             std::size_t history)
    : impl_(std::make_unique<Impl>(columns, rows, history)) {}
TerminalModel::~TerminalModel() = default;
std::size_t TerminalModel::columns() const noexcept {
    return impl_->columns;
}
std::size_t TerminalModel::rows() const noexcept {
    return impl_->rows;
}
std::size_t TerminalModel::historySize() const noexcept {
    return impl_->active().memory.size() - rows();
}
bool TerminalModel::alternateScreen() const noexcept {
    return impl_->useAlternate;
}

const FChar &TerminalModel::cell(std::size_t column, std::size_t row) const {
    const auto &screen = impl_->active();
    if (row >= rows()) {
        throw std::out_of_range("Terminal screen row");
    }
    return screen.memory.at(column, impl_->start(screen) + row);
}

CursorState TerminalModel::cursor() const noexcept {
    return impl_->active().cursor;
}
void TerminalModel::setCursor(CursorState cursor) {
    auto &screen = impl_->active();
    screen.cursor = cursor;
    moveCursor(cursor.column, cursor.row);
}
void TerminalModel::moveCursor(int column, int row) {
    auto &screen = impl_->active();
    screen.cursor.column = std::clamp(column, 0, int(columns()) - 1);
    screen.cursor.row = std::clamp(row, 0, int(rows()) - 1);
    screen.wrapPending = false;
}
void TerminalModel::setColors(Rgb8 foreground, Rgb8 background) {
    auto &screen = impl_->active();
    screen.style.setForeground(ColorReference::rgb(foreground));
    screen.style.setBackground(ColorReference::rgb(background));
}
void TerminalModel::setColorReferences(ColorReference foreground,
                                       ColorReference background) {
    impl_->active().style.setForeground(foreground);
    impl_->active().style.setBackground(background);
}
void TerminalModel::setAttributes(CellAttribute attributes) {
    impl_->active().style.setStyle(attributes);
}
void TerminalModel::setCursorVisible(bool visible) {
    impl_->active().cursor.visible = visible;
}
void TerminalModel::setCursorAppearance(CursorState::Shape shape, bool blink) {
    impl_->active().cursor.shape = shape;
    impl_->active().cursor.blink = blink;
}
void TerminalModel::saveCursor() {
    auto &screen = impl_->active();
    screen.savedCursor = screen.cursor;
    screen.savedStyle = screen.style;
    screen.savedWrap = screen.wrapPending;
    screen.hasSavedCursor = true;
    screen.savedOrigin = screen.origin;
    screen.savedAutoWrap = screen.autoWrap;
}
void TerminalModel::restoreCursor() {
    auto &screen = impl_->active();
    if (!screen.hasSavedCursor) {
        return;
    }
    screen.cursor = screen.savedCursor;
    screen.cursor.column =
        std::clamp(screen.cursor.column, 0, int(columns()) - 1);
    screen.cursor.row = std::clamp(screen.cursor.row, 0, int(rows()) - 1);
    screen.style = screen.savedStyle;
    screen.origin = screen.savedOrigin;
    screen.autoWrap = screen.savedAutoWrap;
    if (screen.origin) {
        screen.cursor.row = std::clamp(
            screen.cursor.row, screen.scrollTop, screen.scrollBottom);
    }
    screen.wrapPending = screen.savedWrap && screen.autoWrap &&
                         screen.cursor.column == int(columns()) - 1;
}
void TerminalModel::carriageReturn() {
    auto &screen = impl_->active();
    screen.cursor.column = 0;
    screen.wrapPending = false;
}
void TerminalModel::lineFeed() {
    auto &screen = impl_->active();
    screen.wrapPending = false;
    if (screen.cursor.row == screen.scrollBottom) {
        impl_->shiftRows(
            screen, screen.scrollTop, screen.scrollBottom, 1, true, true);
    }
    else if (screen.cursor.row + 1 < int(rows())) {
        ++screen.cursor.row;
    }
}
void TerminalModel::write(std::u32string_view text) {
    auto &screen = impl_->active();
    for (char32_t ch : text) {
        switch (ch) {
        case U'\r':
            carriageReturn();
            continue;
        case U'\n':
            lineFeed();
            continue;
        case U'\b':
            moveCursor(std::max(0, screen.cursor.column - 1),
                       screen.cursor.row);
            continue;
        case U'\t':
            tabForward();
            continue;
        default:
            break;
        }
        if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f)) {
            continue;
        }
        if (ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) {
            ch = U'\ufffd';
        }
        if (screen.wrapPending) {
            const auto wrapped = screen.firstRow + impl_->start(screen) +
                                 std::size_t(screen.cursor.row);
            const auto epoch = impl_->selectionEpoch;
            const int previousRow = screen.cursor.row;
            carriageReturn();
            lineFeed();
            if (epoch != impl_->selectionEpoch) {
                // A scrolling region moved the old cursor line up one row.
                if (previousRow > screen.scrollTop) {
                    screen.softWrap[impl_->start(screen) +
                                    std::size_t(previousRow - 1)] = true;
                }
            }
            else if (wrapped >= screen.firstRow &&
                     wrapped + 1 < screen.firstRow + screen.memory.size()) {
                screen.softWrap[wrapped - screen.firstRow] = true;
            }
        }
        if (screen.insert) {
            insertCharacters(1);
        }
        screen.memory.at(std::size_t(screen.cursor.column),
                         impl_->start(screen) +
                             std::size_t(screen.cursor.row)) =
            FChar{ch,
                  screen.style.foreground,
                  screen.style.background,
                  screen.style.attributes};
        if (screen.cursor.column + 1 == int(columns())) {
            screen.wrapPending = screen.autoWrap;
        }
        else {
            ++screen.cursor.column;
        }
    }
}
void TerminalModel::eraseLine(EraseMode mode) {
    auto &screen = impl_->active();
    const auto x = std::size_t(screen.cursor.column);
    impl_->fillRow(screen,
                   std::size_t(screen.cursor.row),
                   mode == EraseMode::ToEnd ? x : 0,
                   mode == EraseMode::ToStart ? x + 1 : columns());
    screen.wrapPending = false;
}
void TerminalModel::eraseDisplay(EraseMode mode) {
    auto &screen = impl_->active();
    const auto y = std::size_t(screen.cursor.row);
    eraseLine(mode);
    if (mode != EraseMode::ToEnd) {
        for (std::size_t row = 0; row < y; ++row) {
            impl_->fillRow(screen, row, 0, columns());
        }
    }
    if (mode != EraseMode::ToStart) {
        for (std::size_t row = y + 1; row < rows(); ++row) {
            impl_->fillRow(screen, row, 0, columns());
        }
    }
}
void TerminalModel::eraseCharacters(int count) {
    if (count <= 0) {
        return;
    }
    auto &screen = impl_->active();
    const auto x = std::size_t(screen.cursor.column);
    impl_->fillRow(screen,
                   std::size_t(screen.cursor.row),
                   x,
                   x + std::min(std::size_t(count), columns() - x));
    screen.wrapPending = false;
}
void TerminalModel::insertCharacters(int count) {
    if (count <= 0) {
        return;
    }
    auto &screen = impl_->active();
    const auto x = std::size_t(screen.cursor.column);
    const auto size = std::min(std::size_t(count), columns() - x);
    auto *row = &screen.memory.at(
        0, impl_->start(screen) + std::size_t(screen.cursor.row));
    const auto moving = columns() - x - size;
    if (moving) {
        std::memmove(row + x + size, row + x, moving * sizeof(FChar));
    }
    impl_->fillRow(screen, std::size_t(screen.cursor.row), x, x + size);
    screen.wrapPending = false;
}
void TerminalModel::deleteCharacters(int count) {
    if (count <= 0) {
        return;
    }
    auto &screen = impl_->active();
    const auto x = std::size_t(screen.cursor.column);
    const auto size = std::min(std::size_t(count), columns() - x);
    auto *row = &screen.memory.at(
        0, impl_->start(screen) + std::size_t(screen.cursor.row));
    const auto moving = columns() - x - size;
    if (moving) {
        std::memmove(row + x, row + x + size, moving * sizeof(FChar));
    }
    impl_->fillRow(
        screen, std::size_t(screen.cursor.row), columns() - size, columns());
    screen.wrapPending = false;
}
void TerminalModel::insertLines(int count) {
    auto &screen = impl_->active();
    if (screen.cursor.row >= screen.scrollTop &&
        screen.cursor.row <= screen.scrollBottom) {
        impl_->shiftRows(screen,
                         screen.cursor.row,
                         screen.scrollBottom,
                         count,
                         false,
                         false);
    }
    screen.wrapPending = false;
}
void TerminalModel::deleteLines(int count) {
    auto &screen = impl_->active();
    if (screen.cursor.row >= screen.scrollTop &&
        screen.cursor.row <= screen.scrollBottom) {
        impl_->shiftRows(
            screen, screen.cursor.row, screen.scrollBottom, count, true, false);
    }
    screen.wrapPending = false;
}
void TerminalModel::scrollUp(int count) {
    auto &screen = impl_->active();
    impl_->shiftRows(
        screen, screen.scrollTop, screen.scrollBottom, count, true, true);
    screen.wrapPending = false;
}
void TerminalModel::scrollDown(int count) {
    auto &screen = impl_->active();
    impl_->shiftRows(
        screen, screen.scrollTop, screen.scrollBottom, count, false, false);
    screen.wrapPending = false;
}
void TerminalModel::reverseIndex() {
    auto &screen = impl_->active();
    if (screen.cursor.row == screen.scrollTop) {
        impl_->shiftRows(
            screen, screen.scrollTop, screen.scrollBottom, 1, false, false);
    }
    else if (screen.cursor.row > 0) {
        --screen.cursor.row;
    }
    screen.wrapPending = false;
}
bool TerminalModel::setScrollRegion(int top, int bottom) {
    if (top < 0 || bottom >= int(rows()) || top > bottom ||
        (top == bottom && rows() != 1)) {
        return false;
    }
    auto &screen = impl_->active();
    screen.scrollTop = top;
    screen.scrollBottom = bottom;
    addressCursor(0, 0);
    return true;
}
int TerminalModel::scrollTop() const noexcept {
    return impl_->active().scrollTop;
}
int TerminalModel::scrollBottom() const noexcept {
    return impl_->active().scrollBottom;
}
void TerminalModel::setOriginMode(bool enabled) {
    impl_->active().origin = enabled;
    addressCursor(0, 0);
}
bool TerminalModel::originMode() const noexcept {
    return impl_->active().origin;
}
void TerminalModel::addressCursor(int column, int row) {
    const auto &screen = impl_->active();
    const int top = screen.origin ? screen.scrollTop : 0;
    const int bottom = screen.origin ? screen.scrollBottom : int(rows()) - 1;
    const auto y = std::clamp(
        std::int64_t(row) + top, std::int64_t(top), std::int64_t(bottom));
    moveCursor(column, int(y));
}
void TerminalModel::setAutoWrap(bool enabled) {
    auto &screen = impl_->active();
    if (screen.autoWrap != enabled) {
        screen.wrapPending = false;
    }
    screen.autoWrap = enabled;
}
void TerminalModel::setInsertMode(bool enabled) {
    impl_->active().insert = enabled;
}
void TerminalModel::tabForward(int count) {
    if (count <= 0) {
        return;
    }
    const auto cursor = this->cursor();
    for (std::size_t x = std::size_t(cursor.column) + 1; x < columns(); ++x) {
        if (impl_->tabStops[x] && --count == 0) {
            moveCursor(int(x), cursor.row);
            return;
        }
    }
    moveCursor(int(columns()) - 1, cursor.row);
}
void TerminalModel::tabBackward(int count) {
    if (count <= 0) {
        return;
    }
    const auto cursor = this->cursor();
    for (int x = cursor.column - 1; x >= 0; --x) {
        if (impl_->tabStops[std::size_t(x)] && --count == 0) {
            moveCursor(x, cursor.row);
            return;
        }
    }
    moveCursor(0, cursor.row);
}
void TerminalModel::setTabStop() {
    impl_->tabStops[std::size_t(cursor().column)] = true;
}
void TerminalModel::clearTabStops(bool all) {
    if (all) {
        std::fill(impl_->tabStops.begin(), impl_->tabStops.end(), false);
    }
    else {
        impl_->tabStops[std::size_t(cursor().column)] = false;
    }
}
void TerminalModel::eraseScrollback() {
    if (impl_->useAlternate || historySize() == 0) {
        return;
    }
    // Rebuild before replacing: allocation failure leaves cells and view
    // intact.
    impl_->primary = impl_->resized(
        *impl_->primary, columns(), rows(), impl_->scrollback, true).screen;
    impl_->segments.clear(); // The history is gone, the rows waiting to join it too.
    ++impl_->selectionEpoch;
}

void TerminalModel::reset() {
    auto replacement =
        std::make_unique<Impl>(columns(), rows(), impl_->scrollback);
    replacement->selectionEpoch = impl_->selectionEpoch + 1;
    impl_ = std::move(replacement);
}

void TerminalModel::setAlternateScreen(bool enabled) {
    if (enabled == impl_->useAlternate) {
        return;
    }
    if (enabled) {
        auto screen = std::make_unique<Impl::Screen>(
            columns(), rows(), 0, impl_->blank(*impl_->primary));
        screen->cursor = impl_->primary->cursor;
        screen->cursor.column = 0;
        screen->cursor.row = 0;
        screen->style.attributes = impl_->primary->style.attributes;
        screen->autoWrap = impl_->primary->autoWrap;
        screen->insert = impl_->primary->insert;
        impl_->alternate = std::move(screen);
    }
    impl_->useAlternate = enabled;
    ++impl_->selectionEpoch;
}
void TerminalModel::resize(std::size_t columns, std::size_t rows) {
    validateSize(columns, rows, impl_->scrollback);
    if (columns == this->columns() && rows == this->rows()) {
        return;
    }
    auto primary = impl_->resized(
        *impl_->primary, columns, rows, impl_->scrollback, false, true);
    auto alternate = impl_->resized(*impl_->alternate, columns, rows, 0).screen;
    auto tabs = Impl::defaultTabs(columns);
    for (std::size_t x = 0; x < std::min(columns, this->columns()); ++x) {
        tabs[x] = impl_->tabStops[x];
    }
    impl_->segments.reserve(impl_->segments.size() + 1); // The last step that can throw.
    if (primary.segmentRows > 0) {
        // The old screen keeps the older rows as they are; the new one has the rest.
        impl_->segments.push_back({std::move(impl_->primary), primary.segmentRows});
    }
    impl_->primary = std::move(primary.screen);
    if (!impl_->segments.empty()) {
        // Room in the row numbers for the older rows that will be added above the oldest one.
        impl_->primary->firstRow += impl_->primary->memory.capacity();
    }
    impl_->quiet = 0;
    impl_->alternate = std::move(alternate);
    impl_->tabStops.swap(tabs);
    impl_->columns = columns;
    impl_->rows = rows;
    ++impl_->selectionEpoch;
}

void TerminalModel::setLiveReflow(LiveReflow mode) noexcept {
    impl_->liveMode = mode;
}
TerminalModel::LiveReflow TerminalModel::liveReflow() const noexcept {
    return impl_->liveMode;
}

std::size_t TerminalModel::firstRetainedRow() const noexcept {
    return impl_->active().firstRow;
}
std::size_t TerminalModel::selectionEpoch() const noexcept {
    return impl_->selectionEpoch;
}
std::optional<char32_t> TerminalModel::textCodePoint(
    TextCell cell) const noexcept {
    const auto &screen = impl_->active();
    if (cell.row < screen.firstRow ||
        cell.row - screen.firstRow >= screen.memory.size() ||
        cell.column >= columns()) {
        return std::nullopt;
    }
    return screen.memory.at(cell.column, cell.row - screen.firstRow).codePoint;
}
bool TerminalModel::textRowWrapped(std::size_t row) const noexcept {
    const auto &screen = impl_->active();
    return row >= screen.firstRow &&
           row - screen.firstRow < screen.memory.size() &&
           screen.softWrap[row - screen.firstRow];
}
std::vector<bool> TerminalModel::viewWrapMarks() const {
    const auto &viewport = impl_->active().viewport;
    const auto first = firstRetainedRow() + viewport.firstRow();
    std::vector<bool> marks(viewport.rowCount());
    if (alternateScreen()) {
        return marks; // Full-screen programs place their own lines: nothing
                      // there "continues".
    }
    for (std::size_t i = 0; i < marks.size(); ++i) {
        marks[i] = textRowWrapped(first + i);
    }
    return marks;
}
std::optional<std::string> TerminalModel::copyText(
    TextCell first, TextCell last, std::size_t byteLimit) const {
    if (std::tie(last.row, last.column) < std::tie(first.row, first.column)) {
        std::swap(first, last);
    }
    const auto &screen = impl_->active();
    if (first.row < screen.firstRow ||
        last.row - screen.firstRow >= screen.memory.size() ||
        first.column >= columns() || last.column >= columns()) {
        return std::nullopt;
    }
    std::string text;
    for (std::size_t y = first.row - screen.firstRow;
         y <= last.row - screen.firstRow;
         ++y) {
        const auto line = screen.memory.row(y);
        const bool final = y == last.row - screen.firstRow;
        const auto begin = y == first.row - screen.firstRow ? first.column : 0;
        auto end = final ? last.column + 1 : columns();
        if (!final && !screen.softWrap[y]) {
            while (end > begin && line.data()[end - 1].codePoint == U' ') {
                --end;
            }
        }
        for (auto x = begin; x < end; ++x) {
            char32_t cp = line.data()[x].codePoint;
            const unsigned size = cp < 0x80      ? 1
                                  : cp < 0x800   ? 2
                                  : cp < 0x10000 ? 3
                                                 : 4;
            if (size > byteLimit - text.size()) {
                return std::nullopt;
            }
            constexpr unsigned prefixes[]{0, 0, 0xc0, 0xe0, 0xf0};
            text.push_back(char(prefixes[size] | (cp >> (6 * (size - 1)))));
            for (unsigned shift = size - 1; shift > 0; --shift) {
                text.push_back(char(0x80 | ((cp >> (6 * (shift - 1))) & 0x3f)));
            }
        }
        if (!final && !screen.softWrap[y]) {
            if (text.size() == byteLimit) {
                return std::nullopt;
            }
            text.push_back('\n');
        }
    }
    return text;
}

void TerminalModel::setSnapScrollToRows(bool snap) {
    impl_->primary->viewport.setSnapToRows(snap);
    impl_->alternate->viewport.setSnapToRows(snap);
}
void TerminalModel::scrollBy(double rows) {
    impl_->active().viewport.scrollBy(rows);
}
void TerminalModel::scrollToOldest() {
    impl_->active().viewport.oldest();
}
void TerminalModel::followTail() {
    impl_->active().viewport.followTail();
}
void TerminalModel::advanceViewport(double seconds) {
    impl_->active().viewport.advance(seconds);
    if (!impl_->segments.empty()) {
        impl_->quiet += seconds;
        if (impl_->quiet >= Impl::kQuietSeconds) {
            try {
                impl_->reflowSlice(Impl::kSliceRows);
            }
            catch (const std::bad_alloc &) {
                impl_->segments.clear(); // Out of memory: the oldest history is given up.
            }
        }
    }
}
bool TerminalModel::reflowPending() const noexcept {
    return !impl_->segments.empty();
}
void TerminalModel::finishReflow() {
    while (!impl_->segments.empty()) {
        impl_->reflowSlice(std::numeric_limits<std::size_t>::max());
    }
}
const ScrollViewport &TerminalModel::viewport() const noexcept {
    return impl_->active().viewport;
}

RenderFrame TerminalModel::frame(int cellHeight) const & {
    if (cellHeight <= 0 ||
        rows() > std::size_t(std::numeric_limits<int>::max() / cellHeight)) {
        throw std::invalid_argument("Invalid terminal cell height");
    }
    const auto &screen = impl_->active();
    const auto &viewport = screen.viewport;
    RenderFrame frame{
        screen.memory.view(viewport.firstRow(), viewport.rowCount()),
        screen.cursor};
    frame.cursor.visible = screen.cursor.visible && viewport.atTail();
    frame.verticalOffsetPixels = viewport.pixelOffset(cellHeight);
    frame.textHeightPixels = int(rows()) * cellHeight;
    frame.scrollbar = viewport.scrollbar();
    frame.scrollbar.visible = frame.scrollbar.visible && !alternateScreen();
    return frame;
}
