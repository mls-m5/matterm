#pragma once

// The wrapped-line marker (docs/tasks/wrap-marker.md,
// docs/SCROLLING_AND_REFLOW.md): a row that continues on the next one gets a
// small filled triangle in the pane's right gutter. Pure geometry and the style
// constants, so that the look is decided here and nowhere else; SdlBackend
// draws it.
//
// Where: the scrollbar strip at the pane's right edge (12 px). With marks on,
// the strip holds the mark on its left and the scrollbar thumb on its right
// (narrower), so the two never overlap. For a pane between two others the
// scrollbar sits in the near half of the divider and the mark in the far half.
// Where the strip is too narrow, or a pane has none, nothing is drawn: the grid
// never changes.
#include "rgb8.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace wrapmark {

struct Style {
    int width = 4;               // Triangle width in pixels.
    double heightFraction = 0.4; // Of the cell height.
    int minHeight = 4;           // Pixels, for a very small font.
    Rgb8 color{110,
               122,
               142}; // The scrollbar thumb's colour at rest ("dim foreground").
    int minStripWidth =
        12; // A strip narrower than this keeps its thumb and gets no marks.
    int stripMarkX = 1; // Left edge of the triangle in the strip.
    int stripThumbWidth =
        4; // The thumb, when it shares the strip with the marks.
    int stripThumbMargin = 1; // Space right of the thumb.
};
inline constexpr Style kStyle{};

struct Point {
    float x = 0, y = 0;
};
struct Triangle {
    Point a, b, c; // a and b: the vertical base (top, bottom); c: the point, to
                   // the right.
};

// The mark of the row whose top is `rowTop` (pixels), in a region that starts
// at `x` and is `regionWidth` wide. Empty when it does not fit.
inline std::optional<Triangle> triangleFor(float x,
                                           float rowTop,
                                           int cellHeight,
                                           int regionWidth,
                                           const Style &style = kStyle) {
    if (cellHeight <= 0 || regionWidth < style.width) {
        return std::nullopt;
    }
    const int wanted = std::max(
        style.minHeight, int(std::lround(style.heightFraction * cellHeight)));
    const float height = float(std::min(wanted, cellHeight));
    const float centre = rowTop + float(cellHeight) / 2.0f;
    const float width = float(style.width);
    return Triangle{{x, centre - height / 2.0f},
                    {x, centre + height / 2.0f},
                    {x + width, centre}};
}

// How the scrollbar strip at the pane's own right edge is divided (x relative
// to its left edge).
struct StripLayout {
    int markX = -1; // -1: no marks in this strip.
    int thumbX = 2;
    int thumbWidth = 0;
    bool hasMarks() const noexcept {
        return markX >= 0;
    }
};

inline StripLayout stripLayout(int stripWidth,
                               bool marks,
                               const Style &style = kStyle) {
    if (marks && stripWidth >= style.minStripWidth) {
        const int thumbX =
            stripWidth - style.stripThumbWidth - style.stripThumbMargin;
        return {style.stripMarkX, thumbX, style.stripThumbWidth};
    }
    return {-1, 2, std::max(0, stripWidth - 4)};
}

} // namespace wrapmark
