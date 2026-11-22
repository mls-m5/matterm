#pragma once

// SDL side of the borderless window (docs/TABS_PLAN.md section 16): the hit
// test callback, the copy of the geometry it reads, and the call that installs
// it. The decisions are in src/frontend/windowhit.h (tested without SDL).
#include "windowhit.h"

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace borderless {

// SDL may call the hit test from another thread than the loop's: the loop
// stores the geometry, the callback reads a copy.
class HitState {
public:
    void set(frontend::WindowGeometry geometry) {
        const std::lock_guard<std::mutex> lock(mutex_);
        geometry_ = std::move(geometry);
    }
    frontend::WindowZone classify(int x, int y) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frontend::classifyWindowPoint(geometry_, x, y);
    }

private:
    mutable std::mutex mutex_;
    frontend::WindowGeometry geometry_;
};

inline SDL_HitTestResult toSdl(frontend::WindowZone zone) {
    using frontend::WindowZone;
    switch (zone) {
    case WindowZone::Drag:
        return SDL_HITTEST_DRAGGABLE;
    case WindowZone::ResizeTopLeft:
        return SDL_HITTEST_RESIZE_TOPLEFT;
    case WindowZone::ResizeTop:
        return SDL_HITTEST_RESIZE_TOP;
    case WindowZone::ResizeTopRight:
        return SDL_HITTEST_RESIZE_TOPRIGHT;
    case WindowZone::ResizeRight:
        return SDL_HITTEST_RESIZE_RIGHT;
    case WindowZone::ResizeBottomRight:
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    case WindowZone::ResizeBottom:
        return SDL_HITTEST_RESIZE_BOTTOM;
    case WindowZone::ResizeBottomLeft:
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    case WindowZone::ResizeLeft:
        return SDL_HITTEST_RESIZE_LEFT;
    case WindowZone::Normal:
        break;
    }
    return SDL_HITTEST_NORMAL;
}

inline SDL_HitTestResult hitTest(SDL_Window *,
                                 const SDL_Point *point,
                                 void *data) {
    return toSdl(
        static_cast<const HitState *>(data)->classify(point->x, point->y));
}

// 0 on success; otherwise SDL_GetError() says why (Wayland: not supported).
inline int install(SDL_Window *window, HitState &state) {
    return SDL_SetWindowHitTest(window, hitTest, &state);
}

// The geometry for the hit test from the strip just planned. Window pixels: the
// view works in drawable pixels, which are `scale` times larger on a high-DPI
// screen.
inline frontend::WindowGeometry geometryOf(SDL_Window *window,
                                           const frontend::TabStrip &strip,
                                           int stripHeightDrawable,
                                           int cellWidthDrawable,
                                           int drawableWidth) {
    frontend::WindowGeometry g;
    SDL_GetWindowSize(window, &g.width, &g.height);
    const double scale = g.width > 0 ? double(drawableWidth) / g.width : 1.0;
    g.stripHeight =
        int(std::lround(stripHeightDrawable / (scale > 0 ? scale : 1.0)));
    g.maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    g.drag = frontend::dragSpans(strip, cellWidthDrawable, scale);
    return g;
}

inline void toggleMaximize(SDL_Window *window) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) {
        SDL_RestoreWindow(window);
    }
    else {
        SDL_MaximizeWindow(window);
    }
}

} // namespace borderless
