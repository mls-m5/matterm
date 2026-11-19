#pragma once

#include "rgb8.h"

#include <cstdint>

// Transient API value, not the packed cell representation. FChar stores the
// payload in its existing RGB bytes and the kind in attribute flag bits.
struct ColorReference {
    enum class Kind : std::uint8_t { Rgb = 0, Indexed = 1, Default = 2 };
    Rgb8 value{0, 0, 0};
    Kind kind = Kind::Default;

    static constexpr ColorReference rgb(Rgb8 value) noexcept {
        return {value, Kind::Rgb};
    }
    static constexpr ColorReference indexed(std::uint8_t index) noexcept {
        return {{index, 0, 0}, Kind::Indexed};
    }
    static constexpr ColorReference defaultColor() noexcept {
        return {};
    }
};
