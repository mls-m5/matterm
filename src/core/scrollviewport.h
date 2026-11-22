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
    // Off (default): the view stops wherever the input left it, also in the middle of a row
    // (pixel scrolling). On: once the input has paused, the target settles on a whole row.
    void setSnapToRows(bool snap) noexcept {
        snap_ = snap;
    }
    bool snapToRows() const noexcept {
        return snap_;
    }
    void rowsAppended(std::size_t count, std::size_t retainedRows);
    // Height growth makes saved rows part of the live screen. Keep historical
    // views anchored; offsets near the old tail become zero, not overscroll.
    void rowsRevealed(std::size_t count,
                      std::size_t retainedRows,
                      std::size_t visibleRows);

    // Older rows were added above the oldest one: positions are measured from the bottom, so
    // the view stays where it is; only the room above grows.
    void rowsPrepended(std::size_t retainedRows);
    // The rows were laid out again: show `firstRow` (an index into the new retained rows) at the
    // top, or stay at the tail when the view was following it.
    void reanchor(std::size_t firstRow, std::size_t retainedRows, std::size_t visibleRows);

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
    bool snap_ = false;
    double quiet_ = 0; // Seconds since the last scrollBy.
};
