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

inline constexpr int kResizeEdge =
    6; // Window pixels along each side that resize.
inline constexpr int kResizeCorner =
    12; // A corner reaches this far along each side.

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
// columns are one span.
inline std::vector<DragSpan> dragSpans(const TabStrip &strip,
                                       int cellWidth,
                                       double scale) {
    std::vector<DragSpan> spans;
    if (cellWidth <= 0 || scale <= 0)
        return spans;
    const auto toWindow = [&](int column) {
        return int(std::lround(double(column) * cellWidth / scale));
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
