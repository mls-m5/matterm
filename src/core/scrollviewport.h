#pragma once

#include "scrollbarstate.h"

#include <cstddef>

// Presentation state only: does not own cells or know about SDL.
// Positions are fractional rows measured upwards from the live bottom.
class ScrollViewport {
public:
    void setGeometry(std::size_t retainedRows, std::size_t visibleRows);
    void scrollBy(double rows);
    void oldest();
    void followTail();
    void advance(double seconds);
    void rowsAppended(std::size_t count, std::size_t retainedRows);

    double offset() const noexcept {
        return displayed_;
    }
    double targetOffset() const noexcept {
        return target_;
    }
    double maxOffset() const noexcept;
    bool atTail() const noexcept;
    std::size_t firstRow() const noexcept;
    std::size_t rowCount() const noexcept;
    float pixelOffset(int cellHeight) const noexcept;
    ScrollbarState scrollbar() const noexcept;

private:
    void clamp() noexcept;
    double firstPosition() const noexcept;
    std::size_t retained_ = 0;
    std::size_t visible_ = 0;
    double target_ = 0;
    double displayed_ = 0;
};
