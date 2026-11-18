#pragma once

#include "cellgridview.h"

#include <string_view>
#include <vector>

// A small CPU-only cell surface, not a terminal parser or scrollback store.
class CellBuffer {
public:
    explicit CellBuffer(std::size_t columns, std::size_t rows);
    CellBuffer(const CellBuffer &) = delete;
    CellBuffer &operator=(const CellBuffer &) = delete;
    CellBuffer(CellBuffer &&) = delete;
    CellBuffer &operator=(CellBuffer &&) = delete;

    std::size_t columns() const noexcept {
        return columns_;
    }
    std::size_t rows() const noexcept {
        return rowViews_.size();
    }

    FChar &at(std::size_t x, std::size_t y);
    const FChar &at(std::size_t x, std::size_t y) const;

    void clear(FChar fill = {});

    // Preserves the top-left intersection; new cells use fill. No text reflow.
    // Invalidates all previously borrowed views/references on success.
    void resize(std::size_t columns, std::size_t rows, FChar fill = {});

    // Coordinates are cells, not pixels. Clip at all edges; do not wrap,
    // scroll, decode UTF-8, or interpret control characters. Each input item
    // uses one cell. Formatted input may overlap this buffer's own cells.
    void draw(int x, int y, FStringView text);
    void draw(int x,
              int y,
              std::u32string_view text,
              Rgb8 foreground = {255, 255, 255},
              Rgb8 background = {0, 0, 0});

    // Borrowed until resize/destruction; cell edits remain visible through it.
    CellGridView view() const &noexcept;
    CellGridView view() const && = delete;

private:
    std::size_t columns_ = 0;
    FString cells_;
    std::vector<FStringView> rowViews_;
};
