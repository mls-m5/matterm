#pragma once

#include <cstdint>

enum class CellAttribute : std::uint16_t {
    None = 0,
    Bold = 1 << 0,
    Italic = 1 << 1,
    Underline = 1 << 2,
    Strike = 1 << 3,
    Faint = 1 << 4,
    Blink = 1 << 5
};
constexpr CellAttribute operator|(CellAttribute a, CellAttribute b) noexcept {
    return CellAttribute(std::uint16_t(a) | std::uint16_t(b));
}
constexpr CellAttribute operator&(CellAttribute a, CellAttribute b) noexcept {
    return CellAttribute(std::uint16_t(a) & std::uint16_t(b));
}
constexpr CellAttribute operator~(CellAttribute value) noexcept {
    return CellAttribute(std::uint16_t(~std::uint16_t(value)));
}
constexpr bool hasAttribute(CellAttribute value, CellAttribute flag) noexcept {
    return (value & flag) != CellAttribute::None;
}
