#pragma once

#include "cellattributes.h"
#include "colorreference.h"
#include "rgb8.h"

#include <cstddef>
#include <type_traits>

// First-pass cell format: one Unicode code point in one column.
// The terminal model will need explicit support for wide cells and clusters.
struct FChar {
    char32_t codePoint = U' ';
    Rgb8 foreground{255, 255, 255};
    Rgb8 background{0, 0, 0};
    CellAttribute attributes = CellAttribute::None;

    constexpr ColorReference foregroundColor() const noexcept {
        return color(foreground, 8);
    }
    constexpr ColorReference backgroundColor() const noexcept {
        return color(background, 10);
    }
    constexpr void setForeground(ColorReference value) noexcept {
        setColor(foreground, value, 8);
    }
    constexpr void setBackground(ColorReference value) noexcept {
        setColor(background, value, 10);
    }
    constexpr CellAttribute style() const noexcept {
        return CellAttribute(std::uint16_t(attributes) & 0xffu);
    }
    constexpr void setStyle(CellAttribute style) noexcept {
        attributes = CellAttribute((std::uint16_t(attributes) & 0xff00u) |
                                   (std::uint16_t(style) & 0xffu));
    }

private:
    constexpr ColorReference color(Rgb8 payload,
                                   unsigned shift) const noexcept {
        return {payload,
                ColorReference::Kind((std::uint16_t(attributes) >> shift) & 3)};
    }
    constexpr void setColor(Rgb8 &payload,
                            ColorReference value,
                            unsigned shift) noexcept {
        payload = value.value;
        attributes =
            CellAttribute((std::uint16_t(attributes) & ~(3u << shift)) |
                          (std::uint16_t(value.kind) << shift));
    }
};

// The style bits occupy the old tail padding; no pointers or per-cell
// allocation.
static_assert(sizeof(FChar) == 12, "Cell layout must remain compact");
static_assert(offsetof(FChar, codePoint) == 0 &&
              offsetof(FChar, foreground) == 4 &&
              offsetof(FChar, background) == 7 &&
              offsetof(FChar, attributes) == 10);
static_assert(std::is_standard_layout_v<FChar> &&
              std::is_trivially_copyable_v<FChar>);
