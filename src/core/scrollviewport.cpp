#include "scrollviewport.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void ScrollViewport::setGeometry(std::size_t retainedRows,
                                 std::size_t visibleRows) {
    retained_ = retainedRows;
    visible_ = visibleRows;
    clamp();
}

double ScrollViewport::maxOffset() const noexcept {
    return double(retained_ - std::min(retained_, visible_));
}

void ScrollViewport::clamp() noexcept {
    target_ = std::clamp(target_, 0.0, maxOffset());
    displayed_ = std::clamp(displayed_, 0.0, maxOffset());
}

void ScrollViewport::scrollBy(double rows) {
    if (!std::isfinite(rows)) {
        throw std::invalid_argument("Non-finite scroll distance");
    }
    // Clamp the delta before adding, avoiding overflow for extreme inputs.
    target_ += std::clamp(rows, -target_, maxOffset() - target_);
}

void ScrollViewport::oldest() {
    target_ = maxOffset();
}
void ScrollViewport::followTail() {
    target_ = 0;
}

void ScrollViewport::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        throw std::invalid_argument("Invalid scroll animation interval");
    }
    // Frame-rate-independent exponential convergence, with a 70 ms time
    // constant.
    displayed_ += (target_ - displayed_) * -std::expm1(-seconds / 0.07);
    if (std::abs(target_ - displayed_) < 0.001) {
        displayed_ = target_;
    }
    clamp();
}

void ScrollViewport::rowsAppended(std::size_t count, std::size_t retainedRows) {
    // Keep both the camera and its destination anchored while reading history.
    // A zero target continues following output, even during return animation.
    if (target_ > 0) {
        target_ += double(count);
        displayed_ += double(count);
    }
    // While returning to the tail, keep easing to zero instead of repeatedly
    // moving the camera away from it as streaming output arrives.
    retained_ = retainedRows;
    clamp();
}

void ScrollViewport::rowsRevealed(std::size_t count,
                                  std::size_t retainedRows,
                                  std::size_t visibleRows) {
    target_ = std::max(0.0, target_ - double(count));
    displayed_ = std::max(0.0, displayed_ - double(count));
    setGeometry(retainedRows, visibleRows);
}

bool ScrollViewport::atTail() const noexcept {
    return target_ == 0 && displayed_ == 0;
}

double ScrollViewport::firstPosition() const noexcept {
    return maxOffset() - displayed_;
}

std::size_t ScrollViewport::firstRow() const noexcept {
    return std::size_t(std::floor(firstPosition()));
}

std::size_t ScrollViewport::rowCount() const noexcept {
    if (visible_ == 0) {
        return 0;
    }
    const auto remaining = retained_ - firstRow();
    const auto count = std::min(visible_, remaining);
    return count < remaining && firstPosition() > double(firstRow()) ? count + 1
                                                                     : count;
}

float ScrollViewport::pixelOffset(int cellHeight) const noexcept {
    const float pixels =
        -float(firstPosition() - double(firstRow())) * float(cellHeight);
    // Float conversion must not turn a fractional row into a whole-row offset.
    return std::max(pixels, -std::nextafter(float(cellHeight), 0.0f));
}

ScrollbarState ScrollViewport::scrollbar() const noexcept {
    return {maxOffset() > 0,
            maxOffset() > 0 ? float(firstPosition() / maxOffset()) : 0.0f,
            retained_ > 0
                ? float(std::min(visible_, retained_)) / float(retained_)
                : 1.0f};
}
