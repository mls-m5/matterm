#pragma once

// Paint-only native decoration. Does not change pane layout, scrollbar tracks,
// cells or hit targets. Drawable pixels (not window/logical pixels).
#include "tabtypes.h"
#include "windowhit.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace frontend {
struct SplitDecoration {
    std::vector<PixelRect> edges, middle, outer;
};
inline Rgb8 splitMiddleColor(const Palette &palette) {
    return borderColor(palette);
}
inline Rgb8 splitEdgeColor(const Palette &palette) {
    const auto c = splitMiddleColor(palette);
    return {static_cast<unsigned char>(c.r / 2),
            static_cast<unsigned char>(c.g / 2),
            static_cast<unsigned char>(c.b / 2)};
}
namespace splitdecoration_detail {
inline PixelRect intersection(const PixelRect &a, const PixelRect &b) {
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return {};
    const auto x = std::max<std::int64_t>(a.x, b.x),
               y = std::max<std::int64_t>(a.y, b.y);
    const auto right =
        std::min(std::int64_t(a.x) + a.w, std::int64_t(b.x) + b.w);
    const auto bottom =
        std::min(std::int64_t(a.y) + a.h, std::int64_t(b.y) + b.h);
    if (right <= x || bottom <= y)
        return {};
    return {int(x), int(y), int(right - x), int(bottom - y)};
}
} // namespace splitdecoration_detail

inline SplitDecoration splitDecoration(const std::vector<DividerBox> &dividers,
                                       PixelRect content,
                                       bool ownFrame,
                                       bool maximized,
                                       bool fullscreen) {
    using splitdecoration_detail::intersection;
    SplitDecoration out;
    if (content.x < 0 || content.y < 0 || content.w <= 0 || content.h <= 0 ||
        std::int64_t(content.x) + content.w > std::numeric_limits<int>::max() ||
        std::int64_t(content.y) + content.h > std::numeric_limits<int>::max())
        return out;
    const bool edgeToEdge = maximized || fullscreen;
    const bool sides = !ownFrame && !edgeToEdge;
    const bool top = content.y > 0 || sides;
    const auto add =
        [&](std::vector<PixelRect> &layer, PixelRect r, PixelRect bounds) {
            r = intersection(r, bounds);
            if (r.w > 0 && r.h > 0)
                layer.push_back(r);
        };
    if (top)
        add(out.outer, {content.x, content.y, content.w, 1}, content);
    if (sides) {
        add(out.outer, {content.x, content.y, 1, content.h}, content);
        add(out.outer,
            {content.x + content.w - 1, content.y, 1, content.h},
            content);
        add(out.outer,
            {content.x, content.y + content.h - 1, content.w, 1},
            content);
    }
    // Do not let themed divider ends overwrite the thin outer pixel ring.
    PixelRect inner{content.x + int(sides),
                    content.y + int(top),
                    std::max(0, content.w - 2 * int(sides)),
                    std::max(0, content.h - int(top) - int(sides))};
    std::vector<DividerBox> clipped;
    for (auto d : dividers) {
        d.rect = intersection(d.rect, inner);
        if (d.rect.w <= 0 || d.rect.h <= 0)
            continue;
        clipped.push_back(d);
        out.edges.push_back(d.rect);
        auto middle = d.rect;
        if (d.direction == SplitDirection::SideBySide) {
            ++middle.x;
            middle.w -= 2;
        }
        else {
            ++middle.y;
            middle.h -= 2;
        }
        add(out.middle, middle, inner);
    }
    // All dark bases precede ALL middle fills. Overlap and seam bridges make
    // the middle the union at crosses/Ts, not a per-divider dark stripe.
    for (const auto &v : clipped) {
        if (v.direction != SplitDirection::SideBySide)
            continue;
        for (const auto &h : clipped) {
            if (h.direction != SplitDirection::Stacked)
                continue;
            add(out.middle, intersection(v.rect, h.rect), inner);
            const int vr = v.rect.x + v.rect.w, hb = h.rect.y + h.rect.h;
            const int hr = h.rect.x + h.rect.w, vb = v.rect.y + v.rect.h;
            if (h.rect.x == vr)
                add(out.middle,
                    intersection({vr - 1, h.rect.y + 1, 1, h.rect.h - 2},
                                 v.rect),
                    inner);
            if (hr == v.rect.x)
                add(out.middle,
                    intersection({hr, h.rect.y + 1, 1, h.rect.h - 2}, v.rect),
                    inner);
            if (v.rect.y == hb)
                add(out.middle,
                    intersection({v.rect.x + 1, hb - 1, v.rect.w - 2, 1},
                                 h.rect),
                    inner);
            if (vb == h.rect.y)
                add(out.middle,
                    intersection({v.rect.x + 1, vb, v.rect.w - 2, 1}, h.rect),
                    inner);
        }
    }
    return out;
}
template <class Paint>
inline void paintSplitGutters(const SplitDecoration &plan,
                              const Palette &palette,
                              Paint &&paint) {
    for (const auto &r : plan.edges)
        paint(r, splitEdgeColor(palette));
    for (const auto &r : plan.middle)
        paint(r, splitMiddleColor(palette));
}
} // namespace frontend
