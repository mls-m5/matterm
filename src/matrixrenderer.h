#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Rgb8 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

// First-pass cell format: one Unicode code point in one column.
// The terminal model will need explicit support for wide cells and clusters.
struct FChar {
    char32_t codePoint = U' ';
    Rgb8 foreground{255, 255, 255};
    Rgb8 background{0, 0, 0};
};

using FString = std::vector<FChar>;

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

struct CellGridView {
    // Row-major, tightly packed: cells.size() == columns * rows.
    FStringView cells;
    std::size_t columns = 0;
    std::size_t rows = 0;
};

class MatrixRenderer {
public:
    MatrixRenderer() = default;
    MatrixRenderer(const MatrixRenderer &) = delete;
    MatrixRenderer &operator=(const MatrixRenderer &) = delete;

    // Planned OpenGL entry point. Draws synchronously from the borrowed grid
    // into the current context's framebuffer. SDL handles the buffer swap.
    void drawFrame(CellGridView grid, int drawableWidth, int drawableHeight);
};
