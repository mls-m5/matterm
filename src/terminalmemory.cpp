#include "terminalmemory.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>

TerminalMemory::TerminalMemory(std::size_t columns,
                               std::size_t capacity,
                               std::size_t blockRows)
    : columns_(columns)
    , capacity_(capacity)
    , blockRows_(blockRows) {
    if (capacity == 0 || blockRows == 0) {
        throw std::invalid_argument(
            "History capacity and block size must be positive");
    }
    blocks_ = allocateBlocks(columns, {});
}

TerminalMemory::Blocks TerminalMemory::allocateBlocks(std::size_t columns,
                                                      FChar fill) const {
    if (columns != 0 && capacity_ > FString{}.max_size() / columns) {
        throw std::length_error("TerminalMemory dimensions are too large");
    }
    Blocks blocks;
    blocks.reserve(capacity_ / blockRows_ + (capacity_ % blockRows_ != 0));
    for (std::size_t remaining = capacity_; remaining != 0;) {
        const auto count = std::min(remaining, blockRows_);
        blocks.emplace_back(count * columns, fill);
        remaining -= count;
    }
    return blocks;
}

std::size_t TerminalMemory::physicalRow(std::size_t index) const noexcept {
    // Avoid overflow in head_ + index for very large capacities.
    const auto untilWrap = capacity_ - head_;
    return index < untilWrap ? head_ + index : index - untilWrap;
}

FChar *TerminalMemory::slot(std::size_t physical) noexcept {
    return columns_ ? blocks_[physical / blockRows_].data() +
                          (physical % blockRows_) * columns_
                    : nullptr;
}

const FChar *TerminalMemory::slot(std::size_t physical) const noexcept {
    return columns_ ? blocks_[physical / blockRows_].data() +
                          (physical % blockRows_) * columns_
                    : nullptr;
}

FStringView TerminalMemory::row(std::size_t index) const & {
    if (index >= size_) {
        throw std::out_of_range("TerminalMemory row");
    }
    return FStringView(slot(physicalRow(index)), columns_);
}

std::size_t TerminalMemory::appendSlot() const noexcept {
    return size_ == capacity_ ? head_ : physicalRow(size_);
}

void TerminalMemory::commitAppend() noexcept {
    if (size_ == capacity_) {
        head_ = head_ == capacity_ - 1 ? 0 : head_ + 1;
    }
    else {
        ++size_;
    }
    visibleRows_.clear();
}

void TerminalMemory::append(FStringView text, FChar fill) {
    const auto count = std::min(columns_, text.size());
    FChar *destination = slot(appendSlot());
    if (count != 0) {
        static_assert(std::is_trivially_copyable_v<FChar>);
        std::memmove(destination, text.data(), count * sizeof(FChar));
    }
    if (columns_ != 0) {
        std::fill(destination + count, destination + columns_, fill);
    }
    commitAppend();
}

void TerminalMemory::append(std::u32string_view text,
                            Rgb8 foreground,
                            Rgb8 background) {
    const auto count = std::min(columns_, text.size());
    FChar *destination = slot(appendSlot());
    for (std::size_t x = 0; x < columns_; ++x) {
        destination[x] =
            FChar{x < count ? text[x] : U' ', foreground, background};
    }
    commitAppend();
}

void TerminalMemory::clear() noexcept {
    head_ = 0;
    size_ = 0;
    visibleRows_.clear();
}

void TerminalMemory::resizeColumns(std::size_t columns, FChar fill) {
    if (columns == columns_) {
        return;
    }
    auto blocks = allocateBlocks(columns, fill);
    const auto count = std::min(columns, columns_);
    if (count != 0) {
        for (std::size_t y = 0; y < size_; ++y) {
            auto *destination =
                blocks[y / blockRows_].data() + (y % blockRows_) * columns;
            std::copy_n(slot(physicalRow(y)), count, destination);
        }
    }
    blocks_.swap(blocks);
    columns_ = columns;
    head_ = 0;
    visibleRows_.clear();
}

CellGridView TerminalMemory::view(std::size_t first,
                                  std::size_t count) const & {
    if (first > size_) {
        throw std::out_of_range("TerminalMemory view start");
    }
    count = std::min(count, size_ - first);
    visibleRows_.clear();
    visibleRows_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        visibleRows_.emplace_back(slot(physicalRow(first + i)), columns_);
    }
    return CellGridView(visibleRows_.data(), columns_, count);
}
