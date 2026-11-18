#pragma once

#include "cellgridview.h"

#include <string_view>
#include <vector>

// Bounded row history. Physical row slots form a ring, stored in reusable
// blocks with contiguous cells inside each block. Appending never shifts rows.
// This is storage, not a terminal parser or a complete screen model.
class TerminalMemory {
public:
    TerminalMemory(std::size_t columns,
                   std::size_t capacity,
                   std::size_t blockRows = 64);
    TerminalMemory(const TerminalMemory &) = delete;
    TerminalMemory &operator=(const TerminalMemory &) = delete;
    TerminalMemory(TerminalMemory &&) = delete;
    TerminalMemory &operator=(TerminalMemory &&) = delete;

    std::size_t columns() const noexcept {
        return columns_;
    }
    std::size_t size() const noexcept {
        return size_;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }

    // Index zero is the oldest retained row. Checked access; borrowed cells
    // remain valid only until a mutation or destruction of this memory.
    FStringView row(std::size_t index) const &;
    FStringView row(std::size_t index) const && = delete;

    // Clip to the stored width; fill the remainder. Formatted input may refer
    // to a retained row in this memory, including the row being evicted.
    void append(FStringView text, FChar fill = {});
    void append(std::u32string_view text,
                Rgb8 foreground = {255, 255, 255},
                Rgb8 background = {0, 0, 0});
    void clear() noexcept;

    // Explicit O(retained cells) operation. Preserve rows, clip/fill columns,
    // and do not reflow text. Invalidates views only on success.
    void resizeColumns(std::size_t columns, FChar fill = {});

    // Build only row descriptors, not copies of cells. Count is clipped to the
    // remaining history. first == size() yields an empty view; greater throws.
    // The returned view expires on the next view() call, mutation, or
    // destruction.
    CellGridView view(std::size_t first, std::size_t count) const &;
    CellGridView view(std::size_t first, std::size_t count) const && = delete;

private:
    using Blocks = std::vector<FString>;
    Blocks allocateBlocks(std::size_t columns, FChar fill) const;
    std::size_t physicalRow(std::size_t index) const noexcept;
    FChar *slot(std::size_t physical) noexcept;
    const FChar *slot(std::size_t physical) const noexcept;
    std::size_t appendSlot() const noexcept;
    void commitAppend() noexcept;

    std::size_t columns_;
    std::size_t capacity_;
    std::size_t blockRows_;
    Blocks blocks_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    mutable std::vector<FStringView> visibleRows_;
};
