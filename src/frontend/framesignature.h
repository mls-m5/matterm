#pragma once

#include "palette.h"
#include "renderframe.h"

#include <cstdint>
#include <cstring>

namespace frontend {
// 64-bit FNV-1a over everything that influences the pixels of a frame: cells,
// cursor, offsets, scrollbar, overlays, blink phase and palette. Equal
// signatures for consecutive frames mean redrawing would change nothing, so an
// idle terminal can skip rendering (and the CPU/GPU work behind it). Collisions
// only risk one skipped redraw until the next change.
class FrameSignature {
public:
    template <class T>
    void add(const T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&value, sizeof value);
    }
    void bytes(const void *data, std::size_t size) {
        auto p = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ p[i]) * 1099511628211ull;
        }
    }
    std::uint64_t value() const {
        return hash_;
    }

private:
    std::uint64_t hash_ = 1469598103934665603ull;
};

inline std::uint64_t frameSignature(const RenderFrame &frame,
                                    const Palette &palette) {
    FrameSignature s;
    for (std::size_t y = 0; y < frame.cells.rows(); ++y) {
        const auto row = frame.cells.row(y);
        s.bytes(row.data(), frame.cells.columns() * sizeof(FChar));
    }
    s.add(frame.cells.columns());
    s.add(frame.cells.rows());
    const auto &c = frame.cursor;
    s.add(c.column);
    s.add(c.row);
    s.add(c.shape);
    s.add(c.visible);
    s.add(c.focused);
    s.add(c.color);
    s.add(c.columnOffset);
    s.add(c.rowOffset);
    s.add(frame.verticalOffsetPixels);
    s.add(frame.textHeightPixels);
    s.add(frame.scrollbar.visible);
    s.add(frame.scrollbar.position);
    s.add(frame.scrollbar.size);
    s.add(frame.textBlinkVisible);
    s.add(frame.selectionBegin);
    s.add(frame.selectionEnd);
    s.add(frame.underlineBegin);
    s.add(frame.underlineEnd);
    s.bytes(palette.colors.data(), sizeof palette.colors);
    s.add(palette.foreground);
    s.add(palette.background);
    return s.value();
}
} // namespace frontend
