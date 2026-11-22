#pragma once

// SDL side of the borderless window (docs/TABS_PLAN.md section 16): the hit
// test callback, the copy of the geometry it reads, and the call that installs
// it. The decisions are in src/frontend/windowhit.h (tested without SDL).
#include "framedetect.h"
#include "windowhit.h"

#include <SDL.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <utility>
#include <vector>

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

// Everything written to stderr while it lives is kept (in a temporary file, so a chatty library
// cannot block on a full pipe) and written again to the real stderr by finish(), so nothing is
// hidden from the person who started the program. Used round SDL_Init: libdecor says there
// "No plugins found" when the window will have no frame.
class StderrCapture {
public:
    StderrCapture() {
        (void)std::fflush(stderr);
        file_ = std::tmpfile();
        saved_ = ::dup(2);
        if (file_ == nullptr || saved_ < 0 || ::dup2(::fileno(file_), 2) < 0) {
            release();
            return;
        }
        active_ = true;
    }
    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;
    ~StderrCapture() { finish(); }

    std::string finish() {
        if (!active_) {
            return {};
        }
        active_ = false;
        (void)std::fflush(stderr);
        ::dup2(saved_, 2);
        std::string text;
        std::rewind(file_);
        char buffer[1024];
        for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file_)) > 0;) {
            text.append(buffer, n);
        }
        release();
        if (!text.empty()) {
            (void)std::fwrite(text.data(), 1, text.size(), stderr);
        }
        return text;
    }

private:
    void release() {
        if (saved_ >= 0) ::close(saved_);
        if (file_ != nullptr) (void)std::fclose(file_);
        saved_ = -1;
        file_ = nullptr;
    }
    std::FILE *file_ = nullptr;
    int saved_ = -1;
    bool active_ = false;
};

// libdecor libraries (gtk, cairo) in the plugin folders; -1 when there is no folder to look in.
inline int libdecorPluginFiles() {
    std::vector<std::filesystem::path> dirs;
    if (const char *own = std::getenv("LIBDECOR_PLUGIN_DIR"); own && *own) {
        dirs.emplace_back(own);
    }
    for (const char *dir : {"/usr/lib/x86_64-linux-gnu/libdecor/plugins-1", "/usr/lib/aarch64-linux-gnu/libdecor/plugins-1",
                            "/usr/lib64/libdecor/plugins-1", "/usr/lib/libdecor/plugins-1",
                            "/usr/local/lib/libdecor/plugins-1", "/usr/local/lib/x86_64-linux-gnu/libdecor/plugins-1"}) {
        dirs.emplace_back(dir);
    }
    int found = -1;
    for (const auto &dir : dirs) {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error)) {
            continue;
        }
        found = std::max(found, 0);
        for (const auto &entry : std::filesystem::directory_iterator(dir, error)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("libdecor-", 0) == 0 && name.find(".so") != std::string::npos) {
                ++found;
            }
        }
    }
    return found;
}

// What can be known about the frame of `window` (made with a frame, shown, after its first events).
inline frontend::FrameProbe probeFrame(SDL_Window *window, const std::string &startupMessages) {
    frontend::FrameProbe probe;
    probe.videoDriver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
    probe.bordersKnown =
        SDL_GetWindowBordersSize(window, &probe.top, &probe.left, &probe.bottom, &probe.right) == 0;
    probe.pluginFiles = libdecorPluginFiles();
    probe.libdecorSaidNoPlugins = startupMessages.find("No plugins found") != std::string::npos;
    return probe;
}

// Lets the window system answer the first requests (configure events) so the borders are known.
inline void settleWindow(int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 10) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

// MATTERM_FRAME_DETECT=off|borders|plugins|message|any (default off until the owner has chosen).
inline frontend::DetectVariant detectVariantFromEnvironment() {
    frontend::DetectVariant variant = frontend::DetectVariant::Off;
    if (const char *value = std::getenv("MATTERM_FRAME_DETECT"); value && *value) {
        if (!frontend::parseDetectVariant(value, variant)) {
            (void)std::fprintf(stderr, "matterm: MATTERM_FRAME_DETECT=%s is not off, borders, plugins, message or any\n", value);
            variant = frontend::DetectVariant::Off;
        }
    }
    return variant;
}

} // namespace borderless
