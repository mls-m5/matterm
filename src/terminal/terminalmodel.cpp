#include "terminalmodel.h"

#include "terminalmemory.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

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
        CursorState cursor;
        FChar style;
        bool wrapPending = false;
        CursorState savedCursor;
        FChar savedStyle;
        bool savedWrap = false;
        bool hasSavedCursor = false;

        Screen(std::size_t columns,
               std::size_t rows,
               std::size_t history,
               FChar fill = {})
            : memory(columns, rows + history)
            , style(fill) {
            cursor.visible = true;
            for (std::size_t i = 0; i < rows; ++i) {
                memory.append(FStringView{}, fill);
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

    Impl(std::size_t width, std::size_t height, std::size_t history)
        : columns(width)
        , rows(height)
        , scrollback(history) {
        validateSize(width, height, history);
        primary = std::make_unique<Screen>(width, height, history);
        alternate = std::make_unique<Screen>(width, height, 0);
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
        return FChar{U' ', screen.style.foreground, screen.style.background};
    }

    void fillRow(Screen &screen,
                 std::size_t y,
                 std::size_t begin,
                 std::size_t end) {
        for (std::size_t x = begin; x < end; ++x) {
            screen.memory.at(x, start(screen) + y) = blank(screen);
        }
    }

    std::unique_ptr<Screen> resized(const Screen &old,
                                    std::size_t width,
                                    std::size_t height,
                                    std::size_t history) const {
        auto result =
            std::make_unique<Screen>(width, height, history, blank(old));
        result->memory.clear();
        for (std::size_t y = 0; y < old.memory.size(); ++y) {
            result->memory.append(old.memory.row(y), blank(old));
        }
        for (std::size_t y = rows; y < height; ++y) {
            result->memory.append(FStringView{}, blank(old));
        }
        result->style = old.style;
        result->savedCursor = old.savedCursor;
        result->savedStyle = old.savedStyle;
        result->savedWrap = old.savedWrap;
        result->hasSavedCursor = old.hasSavedCursor;
        result->cursor = old.cursor;
        result->cursor.column = std::min(old.cursor.column, int(width) - 1);
        const auto removed = rows > height ? rows - height : 0;
        const auto oldRow = std::size_t(old.cursor.row);
        result->cursor.row = int(oldRow >= removed ? oldRow - removed : 0);
        result->cursor.row = std::min(result->cursor.row, int(height) - 1);
        result->wrapPending = old.wrapPending && oldRow >= removed;
        if (result->wrapPending && width > columns) {
            // Widening exposes the insertion position after the old last cell.
            result->cursor.column = old.cursor.column + 1;
            result->wrapPending = false;
        }
        result->viewport = old.viewport;
        result->viewport.setGeometry(result->memory.size(), height);
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
    screen.style.foreground = foreground;
    screen.style.background = background;
}
void TerminalModel::setCursorVisible(bool visible) {
    impl_->active().cursor.visible = visible;
}
void TerminalModel::saveCursor() {
    auto &screen = impl_->active();
    screen.savedCursor = screen.cursor;
    screen.savedStyle = screen.style;
    screen.savedWrap = screen.wrapPending;
    screen.hasSavedCursor = true;
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
    screen.wrapPending =
        screen.savedWrap && screen.cursor.column == int(columns()) - 1;
}
void TerminalModel::carriageReturn() {
    auto &screen = impl_->active();
    screen.cursor.column = 0;
    screen.wrapPending = false;
}
void TerminalModel::lineFeed() {
    auto &screen = impl_->active();
    screen.wrapPending = false;
    if (screen.cursor.row + 1 < int(rows())) {
        ++screen.cursor.row;
    }
    else {
        screen.memory.append(FStringView{}, impl_->blank(screen));
        screen.viewport.rowsAppended(1, screen.memory.size());
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
            moveCursor(
                int(std::min(columns() - 1,
                             (std::size_t(screen.cursor.column) / 8 + 1) * 8)),
                screen.cursor.row);
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
            carriageReturn();
            lineFeed();
        }
        screen.memory.at(std::size_t(screen.cursor.column),
                         impl_->start(screen) +
                             std::size_t(screen.cursor.row)) =
            FChar{ch, screen.style.foreground, screen.style.background};
        if (screen.cursor.column + 1 == int(columns())) {
            screen.wrapPending = true;
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
        impl_->alternate = std::move(screen);
    }
    impl_->useAlternate = enabled;
}
void TerminalModel::resize(std::size_t columns, std::size_t rows) {
    validateSize(columns, rows, impl_->scrollback);
    if (columns == this->columns() && rows == this->rows()) {
        return;
    }
    auto primary =
        impl_->resized(*impl_->primary, columns, rows, impl_->scrollback);
    auto alternate = impl_->resized(*impl_->alternate, columns, rows, 0);
    impl_->primary = std::move(primary);
    impl_->alternate = std::move(alternate);
    impl_->columns = columns;
    impl_->rows = rows;
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
    frame.scrollbar.visible = !alternateScreen();
    return frame;
}
