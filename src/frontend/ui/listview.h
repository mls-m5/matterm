#pragma once

// The selection and scroll position of a list of `count` rows shown `visible`
// at a time (command palette, help view). No drawing.
#include <algorithm>
#include <cstddef>

namespace frontend::ui {

class ListView {
public:
    std::size_t count() const {
        return count_;
    }
    std::size_t visible() const {
        return visible_;
    }
    std::size_t selected() const {
        return selected_;
    }
    std::size_t first() const {
        return first_;
    }

    // New contents: the selection returns to the top.
    void setCount(std::size_t count) {
        count_ = count;
        selected_ = 0;
        first_ = 0;
    }
    void setVisible(std::size_t rows) {
        visible_ = std::max<std::size_t>(rows, 1);
        reveal();
    }

    void up() {
        select(selected_ > 0 ? selected_ - 1 : (count_ ? count_ - 1 : 0)); // Wraps.
    }
    void down() {
        select(selected_ + 1 < count_ ? selected_ + 1 : 0);
    }
    void pageUp() {
        select(selected_ > visible_ ? selected_ - visible_ : 0);
    }
    void pageDown() {
        select(count_ ? std::min(selected_ + visible_, count_ - 1) : 0);
    }
    void select(std::size_t index) {
        selected_ = count_ ? std::min(index, count_ - 1) : 0;
        reveal();
    }
    // The mouse wheel: scrolls the window without moving the selection unless it
    // would leave the window. Positive: towards the end.
    void scroll(int rows) {
        const std::size_t most = count_ > visible_ ? count_ - visible_ : 0;
        const long long wanted = static_cast<long long>(first_) + rows;
        first_ = static_cast<std::size_t>(std::clamp<long long>(wanted, 0, static_cast<long long>(most)));
        if (selected_ < first_) {
            selected_ = first_;
        }
        else if (selected_ >= first_ + visible_) {
            selected_ = first_ + visible_ - 1;
        }
        if (count_ && selected_ >= count_) {
            selected_ = count_ - 1;
        }
    }

private:
    void reveal() {
        if (selected_ < first_) {
            first_ = selected_;
        }
        else if (selected_ >= first_ + visible_) {
            first_ = selected_ + 1 - visible_;
        }
    }

    std::size_t count_ = 0;
    std::size_t visible_ = 1;
    std::size_t selected_ = 0;
    std::size_t first_ = 0;
};

} // namespace frontend::ui
