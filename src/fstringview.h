#pragma once

#include "fstring.h"

#include <cstddef>

// Borrows contiguous cells; the owner must keep their storage alive and stable.
class FStringView {
public:
    constexpr FStringView() noexcept = default;
    constexpr FStringView(const FChar *data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    explicit FStringView(const FString &cells) noexcept
        : FStringView(cells.data(), cells.size()) {}
    FStringView(const FString &&) = delete;

    constexpr const FChar *data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }

private:
    const FChar *data_ = nullptr;
    std::size_t size_ = 0;
};
