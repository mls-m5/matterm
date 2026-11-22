#include "terminalmodel.h"

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

    std::unique_ptr<Screen> resized(const Screen &old,
                                    std::size_t width,
                                    std::size_t height,
                                    std::size_t history,
                                    bool discardHistory = false) const {
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
        for (std::size_t y = discardHistory ? start(old) : 0;
             y < old.memory.size() - dropped;
             ++y) {
            result->append(old.memory.row(y),
                           blank(old),
                           width == columns && old.softWrap[y]);
        }
        for (std::size_t y = rows + pulled; y < height; ++y) {
            result->append(FStringView{}, blank(old));
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
        return result;
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
        *impl_->primary, columns(), rows(), impl_->scrollback, true);
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
    auto primary =
        impl_->resized(*impl_->primary, columns, rows, impl_->scrollback);
    auto alternate = impl_->resized(*impl_->alternate, columns, rows, 0);
    auto tabs = Impl::defaultTabs(columns);
    for (std::size_t x = 0; x < std::min(columns, this->columns()); ++x) {
        tabs[x] = impl_->tabStops[x];
    }
    impl_->primary = std::move(primary);
    impl_->alternate = std::move(alternate);
    impl_->tabStops.swap(tabs);
    impl_->columns = columns;
    impl_->rows = rows;
    ++impl_->selectionEpoch;
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
