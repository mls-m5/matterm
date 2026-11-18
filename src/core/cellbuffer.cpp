#include "cellbuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace {
struct ClippedRun {
    std::size_t column = 0;
    std::size_t skip = 0;
    std::size_t count = 0;
};

ClippedRun clip(
    int x, int y, std::size_t length, std::size_t columns, std::size_t rows) {
    if (y < 0 || std::size_t(y) >= rows) {
        return {};
    }
    const std::size_t skip = x < 0 ? std::size_t(-std::int64_t(x)) : 0;
    const std::size_t column = x < 0 ? 0 : std::size_t(x);
    if (skip >= length || column >= columns) {
        return {};
    }
    return {column, skip, std::min(length - skip, columns - column)};
}
} // namespace

CellBuffer::CellBuffer(std::size_t columns, std::size_t rows) {
    resize(columns, rows);
}

FChar &CellBuffer::at(std::size_t x, std::size_t y) {
    if (x >= columns_ || y >= rows()) {
        throw std::out_of_range("CellBuffer coordinates");
    }
    return cells_[y * columns_ + x];
}

const FChar &CellBuffer::at(std::size_t x, std::size_t y) const {
    if (x >= columns_ || y >= rows()) {
        throw std::out_of_range("CellBuffer coordinates");
    }
    return cells_[y * columns_ + x];
}

void CellBuffer::clear(FChar fill) {
    std::fill(cells_.begin(), cells_.end(), fill);
}

void CellBuffer::resize(std::size_t columns, std::size_t rowCount, FChar fill) {
    if (columns != 0 && rowCount > cells_.max_size() / columns) {
        throw std::length_error("CellBuffer dimensions are too large");
    }
    // Allocate and build descriptors before committing, so failure preserves
    // the original contents and all existing views.
    FString cells(columns * rowCount, fill);
    std::vector<FStringView> rowViews;
    rowViews.reserve(rowCount);
    for (std::size_t y = 0; y < rowCount; ++y) {
        rowViews.emplace_back(columns ? cells.data() + y * columns : nullptr,
                              columns);
    }
    const auto copyColumns = std::min(columns, columns_);
    if (copyColumns != 0) {
        for (std::size_t y = 0; y < std::min(rowCount, rows()); ++y) {
            std::copy_n(cells_.data() + y * columns_,
                        copyColumns,
                        cells.data() + y * columns);
        }
    }
    cells_.swap(cells);
    rowViews_.swap(rowViews);
    columns_ = columns;
}

void CellBuffer::draw(int x, int y, FStringView text) {
    const auto run = clip(x, y, text.size(), columns_, rows());
    if (run.count == 0) {
        return;
    }
    static_assert(std::is_trivially_copyable_v<FChar>);
    std::memmove(cells_.data() + std::size_t(y) * columns_ + run.column,
                 text.data() + run.skip,
                 run.count * sizeof(FChar));
}

void CellBuffer::draw(
    int x, int y, std::u32string_view text, Rgb8 foreground, Rgb8 background) {
    const auto run = clip(x, y, text.size(), columns_, rows());
    for (std::size_t i = 0; i < run.count; ++i) {
        cells_[std::size_t(y) * columns_ + run.column + i] =
            FChar{text[run.skip + i], foreground, background};
    }
}

CellGridView CellBuffer::view() const &noexcept {
    return CellGridView(rowViews_.data(), columns_, rows());
}
