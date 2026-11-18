#pragma once

#include "fstringview.h"

#include <cstddef>
#include <stdexcept>

// Borrows row descriptors and their cells. Both must remain alive and stable.
// Each row must contain at least columns cells; rows need not be adjacent.
class CellGridView {
public:
    constexpr CellGridView() noexcept = default;
    constexpr CellGridView(const FStringView *rows,
                           std::size_t columns,
                           std::size_t rowCount) noexcept
        : rows_(rows)
        , columns_(columns)
        , rowCount_(rowCount) {}

    constexpr std::size_t columns() const noexcept {
        return columns_;
    }
    constexpr std::size_t rows() const noexcept {
        return rowCount_;
    }

    FStringView row(std::size_t y) const {
        if (y >= rowCount_) {
            throw std::out_of_range("CellGridView row");
        }
        return FStringView(rows_[y].data(), columns_);
    }

private:
    const FStringView *rows_ = nullptr;
    std::size_t columns_ = 0;
    std::size_t rowCount_ = 0;
};
