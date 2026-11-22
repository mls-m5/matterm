#pragma once

// What the native window decides for a borderless window (docs/TABS_PLAN.md
// section 16): which points of the window resize it, which move it, and whether
// the borderless frame is kept at all. Pure data in, answers out: no SDL, so it
// is tested without a display. The native loop turns WindowZone into
// SDL_HitTestResult and hands SDL a copy of the geometry (SDL may ask from
// another thread).
#include "tabstrip.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace frontend {

inline constexpr int kResizeEdge = 8;    // Window pixels along each side that resize (the grip).
inline constexpr int kResizeCorner = 16; // A corner reaches this far along each side.
inline constexpr int kBorderWidth = 4;   // The visible border; narrower than the grip.

enum class WindowZone {
    Normal, // An ordinary point: the click goes to the app.
    Drag,   // The strip's handle: the window manager moves the window.
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft,
};

// Window pixels [begin, end) of the strip row that move the window.
struct DragSpan {
    int begin = 0;
    int end = 0;
};

struct WindowGeometry {
    int width = 0; // Window pixels (not drawable pixels).
    int height = 0;
    int stripHeight = 0; // Height of the strip row, window pixels.
    bool maximized = false;
    std::vector<DragSpan> drag;
};

inline WindowZone classifyWindowPoint(const WindowGeometry &g, int x, int y) {
    if (g.width <= 0 || g.height <= 0 || x < 0 || y < 0 || x >= g.width ||
        y >= g.height) {
        return WindowZone::Normal;
    }
    if (!g.maximized) { // A maximised window belongs to the window manager.
        const bool left = x < kResizeEdge, right = x >= g.width - kResizeEdge;
        const bool top = y < kResizeEdge, bottom = y >= g.height - kResizeEdge;
        const bool nearLeft = x < kResizeCorner,
                   nearRight = x >= g.width - kResizeCorner;
        const bool nearTop = y < kResizeCorner,
                   nearBottom = y >= g.height - kResizeCorner;
        if ((left && nearTop) || (top && nearLeft))
            return WindowZone::ResizeTopLeft;
        if ((right && nearTop) || (top && nearRight))
            return WindowZone::ResizeTopRight;
        if ((left && nearBottom) || (bottom && nearLeft))
            return WindowZone::ResizeBottomLeft;
        if ((right && nearBottom) || (bottom && nearRight))
            return WindowZone::ResizeBottomRight;
        if (top)
            return WindowZone::ResizeTop;
        if (bottom)
            return WindowZone::ResizeBottom;
        if (left)
            return WindowZone::ResizeLeft;
        if (right)
            return WindowZone::ResizeRight;
    }
    if (y < g.stripHeight) {
        for (const DragSpan &span : g.drag) {
            if (x >= span.begin && x < span.end)
                return WindowZone::Drag;
        }
    }
    return WindowZone::Normal;
}

// The columns of the strip without a region (the gaps between tabs and before
// the buttons), as window pixels. `cellWidth` is in drawable pixels; `scale` is
// drawable pixels per window pixel (2 on a high-DPI screen). Adjacent gap
// columns are one span. `offsetX` (drawable pixels) is where the view starts: the border's width.
inline std::vector<DragSpan> dragSpans(const TabStrip &strip,
                                       int cellWidth,
                                       double scale,
                                       int offsetX = 0) {
    std::vector<DragSpan> spans;
    if (cellWidth <= 0 || scale <= 0)
        return spans;
    const auto toWindow = [&](int column) {
        return int(std::lround((double(column) * cellWidth + offsetX) / scale));
    };
    const int columns = int(strip.cells.size());
    int open = -1;
    for (int column = 0; column <= columns; ++column) {
        const bool gap =
            column < columns && strip.hit(column).kind == StripHit::Kind::None;
        if (gap && open < 0) {
            open = column;
        }
        else if (!gap && open >= 0) {
            spans.push_back({toWindow(open), toWindow(column)});
            open = -1;
        }
    }
    return spans;
}

// A border (kBorderWidth px) round the window, drawn by the app because the window has no frame of its
// own (not when maximised). The four strips cover the outermost pixel ring exactly once.
struct BorderRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// What the border takes from the window: the view (strip, panes, layers) is laid out inside it, so
// the border never covers content. No border at the top, where the strip is the edge of the window,
// and none with the system frame or when maximised.
struct WindowInset {
    int left = 0, right = 0, top = 0, bottom = 0;
};

inline WindowInset windowInset(bool ownFrame, bool maximized) {
    if (!ownFrame || maximized) {
        return {};
    }
    return {kBorderWidth, kBorderWidth, 0, kBorderWidth};
}

// The border: left, right and bottom, kBorderWidth wide, exactly the pixels windowInset takes. The
// rectangles cover them once (the bottom ones run into the corners, the sides stop above them).
inline std::vector<BorderRect> borderRects(int width, int height, bool maximized, int thickness = kBorderWidth) {
    if (maximized || width <= 0 || height <= 0 || thickness <= 0) {
        return {};
    }
    const int bottom = std::max(0, height - thickness);
    std::vector<BorderRect> rects;
    rects.push_back({0, 0, std::min(thickness, width), height}); // Left, the full height.
    const int right = std::max(std::min(thickness, width), width - thickness);
    if (right < width) {
        rects.push_back({right, 0, width - right, height}); // Right.
    }
    const int innerLeft = std::min(thickness, width);
    if (bottom < height && right > innerLeft) {
        rects.push_back({innerLeft, bottom, right - innerLeft, height - bottom}); // Bottom, between the sides.
    }
    return rects;
}

// Exactly the colour of the strip, where the window is dragged (the gaps between the tabs): the
// border then looks like the edge of the strip going round the window. Not a guessed shade.
inline Rgb8 borderColor(const Palette &palette) {
    return tabstrip_detail::colors(palette).stripBg;
}

// The pointer shape over the edges: the same classification as the hit test.
enum class CursorShape {
    Default,
    ResizeEastWest,
    ResizeNorthSouth,
    ResizeNorthwestSoutheast,
    ResizeNortheastSouthwest,
};

inline CursorShape cursorShapeFor(WindowZone zone) {
    switch (zone) {
    case WindowZone::ResizeLeft:
    case WindowZone::ResizeRight: return CursorShape::ResizeEastWest;
    case WindowZone::ResizeTop:
    case WindowZone::ResizeBottom: return CursorShape::ResizeNorthSouth;
    case WindowZone::ResizeTopLeft:
    case WindowZone::ResizeBottomRight: return CursorShape::ResizeNorthwestSoutheast;
    case WindowZone::ResizeTopRight:
    case WindowZone::ResizeBottomLeft: return CursorShape::ResizeNortheastSouthwest;
    case WindowZone::Normal:
    case WindowZone::Drag: break;
    }
    return CursorShape::Default;
}

// What --window-test says about a call. Most window calls return nothing; whether they worked can
// only be seen in the window's flags, and Wayland reports no "minimised" at all. So only an error
// return is FAILED; a call that went through without a visible state change is not a failure.
inline const char *callVerdict(bool returnedOk, bool stateReached) {
    if (!returnedOk) return "FAILED";
    return stateReached ? "ok" : "called, state not reported";
}

// What the command line asks for: nothing (the default is our own frame), --borderless (the same,
// still accepted) or --system-frame.
enum class FrameRequest { Default, Own, System };

inline bool wantsOwnFrame(FrameRequest request) {
    return request != FrameRequest::System;
}

// False when the two flags contradict each other.
inline bool combineFrameFlags(bool borderlessFlag, bool systemFrameFlag, FrameRequest &out) {
    if (borderlessFlag && systemFrameFlag) {
        return false;
    }
    out = systemFrameFlag ? FrameRequest::System : borderlessFlag ? FrameRequest::Own : FrameRequest::Default;
    return true;
}

enum class FrameMode { System, Own };

struct FrameChoice {
    FrameMode mode = FrameMode::System;
    std::string note; // Why --borderless was not kept (one line, no newline);
                      // empty if nothing to say.
};

// No half modes: the own frame stays only if it was asked for and the hit test
// works (without it the window could be neither moved nor resized). `reason` is
// the SDL error of the failed call.
inline FrameChoice chooseFrame(bool borderlessRequested,
                               bool hitTestWorks,
                               const std::string &reason) {
    FrameChoice choice;
    if (!borderlessRequested) {
        return choice;
    }
    if (hitTestWorks) {
        choice.mode = FrameMode::Own;
        return choice;
    }
    std::string clean; // An SDL error is plain text, but it ends up on a
                       // terminal: no control characters.
    for (const char c : reason) {
        clean += (static_cast<unsigned char>(c) < 32 || c == 127) ? ' ' : c;
    }
    if (clean.size() > 160) {
        clean.resize(160);
    }
    choice.note = "matterm: --borderless is not available (" +
                  (clean.empty() ? std::string("unknown reason") : clean) +
                  "); using the system frame";
    return choice;
}

} // namespace frontend
