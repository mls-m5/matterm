#pragma once

// Paint-only native decoration. Does not change pane layout, scrollbar tracks,
// cells or hit targets. Drawable pixels (not window/logical pixels).
#include "tabtypes.h"
#include "windowhit.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
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
inline void subtract(std::vector<PixelRect> &rects, PixelRect mask) {
    std::vector<PixelRect> result;
    for (const auto &r : rects) {
        const auto i = intersection(r, mask);
        if (i.w <= 0 || i.h <= 0) {
            result.push_back(r);
            continue;
        }
        if (i.y > r.y)
            result.push_back({r.x, r.y, r.w, i.y - r.y});
        if (i.y + i.h < r.y + r.h)
            result.push_back({r.x, i.y + i.h, r.w, r.y + r.h - i.y - i.h});
        if (i.x > r.x)
            result.push_back({r.x, i.y, i.x - r.x, i.h});
        if (i.x + i.w < r.x + r.w)
            result.push_back({i.x + i.w, i.y, r.x + r.w - i.x - i.w, i.h});
    }
    rects = std::move(result);
}
} // namespace splitdecoration_detail

// Only the innermost existing own-frame pixel, NEVER a terminal viewport pixel.
inline std::vector<PixelRect> ownFrameDarkEdges(
    PixelRect content,
    const std::vector<DividerBox> &dividers,
    const std::vector<PixelRect> &scrollbars,
    bool ownFrame,
    bool maximized,
    bool fullscreen) {
    if (!ownFrame || content.x < 0 ||
        content.y < 0 || content.w <= 0 || content.h <= 0 ||
        std::int64_t(content.x) + content.w >=
            std::numeric_limits<int>::max() ||
        std::int64_t(content.y) + content.h >= std::numeric_limits<int>::max())
        return {};
    const int right = content.x + content.w, bottom = content.y + content.h;
    std::vector<PixelRect> edges;
    if (!maximized && !fullscreen) {
        edges = {{content.x - 1, content.y, 1, content.h + 1},
                 {right, content.y, 1, content.h + 1},
                 {content.x, bottom, content.w, 1}};
    }
    if (content.y > 0)
        edges.push_back({content.x, content.y - 1, content.w, 1});
    for (const auto &d : dividers) {
        if (d.direction == SplitDirection::SideBySide &&
            d.rect.y <= content.y &&
            std::int64_t(d.rect.y) + d.rect.h > content.y && d.rect.w > 2 &&
            content.y > 0)
            splitdecoration_detail::subtract(
                edges, {d.rect.x + 1, content.y - 1, d.rect.w - 2, 1});
    }
    for (const auto &track : scrollbars) {
        const auto s = splitdecoration_detail::intersection(track, content);
        if (s.w <= 0 || s.h <= 0)
            continue;
        if (s.x + s.w == right)
            splitdecoration_detail::subtract(
                edges, {right, s.y, 1, s.h + int(s.y + s.h == bottom)});
        if (s.y + s.h == bottom)
            splitdecoration_detail::subtract(edges, {s.x, bottom, s.w, 1});
        if (s.y == content.y && content.y > 0)
            splitdecoration_detail::subtract(edges,
                                             {s.x, content.y - 1, s.w, 1});
    }
    return edges;
}

inline SplitDecoration splitDecoration(const std::vector<DividerBox> &dividers,
                                       PixelRect content,
                                       bool /*ownFrame*/,
                                       bool /*maximized*/,
                                       bool /*fullscreen*/) {
    using splitdecoration_detail::intersection;
    SplitDecoration out;
    if (content.x < 0 || content.y < 0 || content.w <= 0 || content.h <= 0 ||
        std::int64_t(content.x) + content.w > std::numeric_limits<int>::max() ||
        std::int64_t(content.y) + content.h > std::numeric_limits<int>::max())
        return out;
    const auto add =
        [&](std::vector<PixelRect> &layer, PixelRect r, PixelRect bounds) {
            r = intersection(r, bounds);
            if (r.w > 0 && r.h > 0)
                layer.push_back(r);
        };
    // Only existing gutter pixels belong to this decoration. A post-pane
    // terminal-area outline overwrites right-strip backgrounds, scrollbar
    // pixels and full-bleed glyphs. Own-frame edges must instead use pixels
    // outside the viewport; system-frame edges belong to the window manager.
    const PixelRect inner = content;
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
