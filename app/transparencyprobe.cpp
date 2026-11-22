// `matterm --window-test`, the transparency part (docs/tasks/csd-window.md step 1): can a window
// of this desktop be see-through, and which way of drawing makes it so? Each variant opens a small
// borderless window for about two seconds showing a half-transparent red margin round a solid
// box (frontend::probeLayout) with a fully transparent rim outside the red, and prints what was
// asked for and what SDL gave. The owner says what he sees: the desktop through the margin?
// Only SDL calls, no new library: OpenGL functions come through SDL_GL_GetProcAddress.
#include "transparencyprobe.h"

#include "probelayout.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace borderless {

inline int probeMilliseconds() {
    const std::string driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
    return driver == "dummy" || driver == "offscreen" ? 100 : 2000; // Nobody looks at those.
}

inline void showFor(int milliseconds, const std::function<void()> &draw) {
    for (int waited = 0; waited < milliseconds; waited += 50) {
        SDL_PumpEvents();
        draw();
        SDL_Delay(50);
    }
}

using ProbeWindow = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;

inline ProbeWindow openProbeWindow(Uint32 extraFlags) {
    return ProbeWindow(SDL_CreateWindow("matterm --window-test (transparency)",
                                        SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED,
                                        360,
                                        240,
                                        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | extraFlags),
                       SDL_DestroyWindow);
}

inline void probeRenderer(std::ostream &out, const char *label, Uint32 rendererFlags) {
    out << "  " << label << ": ";
    ProbeWindow window = openProbeWindow(0);
    if (!window) {
        out << "no window: " << SDL_GetError() << '\n';
        return;
    }
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), -1, rendererFlags), SDL_DestroyRenderer);
    if (!renderer) {
        out << "no renderer: " << SDL_GetError() << '\n';
        return;
    }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer.get(), &info);
    const Uint32 format = SDL_GetWindowPixelFormat(window.get());
    const bool blendOk = SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND) == 0;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer.get(), &w, &h);
    const frontend::ProbeLayout layout = frontend::probeLayout(w, h, 40);
    showFor(probeMilliseconds(), [&] {
        SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_NONE); // Write alpha 0 as it is.
        SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 0);
        SDL_RenderClear(renderer.get());
        SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer.get(), 200, 0, 0, 120);
        const SDL_Rect red{layout.red.x, layout.red.y, layout.red.w, layout.red.h};
        SDL_RenderFillRect(renderer.get(), &red);
        SDL_SetRenderDrawColor(renderer.get(), 30, 34, 42, 255);
        const SDL_Rect box{layout.box.x, layout.box.y, layout.box.w, layout.box.h};
        SDL_RenderFillRect(renderer.get(), &box);
        SDL_RenderPresent(renderer.get());
    });
    out << "renderer '" << info.name << "' (" << ((info.flags & SDL_RENDERER_SOFTWARE) ? "software" : "accelerated")
        << "), window pixel format " << SDL_GetPixelFormatName(format) << " ("
        << (SDL_ISPIXELFORMAT_ALPHA(format) ? "has alpha" : "no alpha") << "), blend mode BLEND "
        << (blendOk ? "ok" : "FAILED") << ", margin cleared to alpha 0, red margin alpha 120/255\n";
}

// OpenGL with an 8 bit alpha channel; the few functions needed are fetched through SDL.
inline void probeOpenGl(std::ostream &out) {
    out << "  OpenGL (SDL_GL_ALPHA_SIZE 8): ";
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    ProbeWindow window = openProbeWindow(SDL_WINDOW_OPENGL);
    if (!window) {
        out << "no window: " << SDL_GetError() << '\n';
        return;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window.get());
    if (context == nullptr) {
        out << "no context: " << SDL_GetError() << '\n';
        return;
    }
    using ClearColor = void (*)(float, float, float, float);
    using Clear = void (*)(unsigned);
    using Enable = void (*)(unsigned);
    using Scissor = void (*)(int, int, int, int);
    const auto clearColor = reinterpret_cast<ClearColor>(SDL_GL_GetProcAddress("glClearColor"));
    const auto clear = reinterpret_cast<Clear>(SDL_GL_GetProcAddress("glClear"));
    const auto enable = reinterpret_cast<Enable>(SDL_GL_GetProcAddress("glEnable"));
    const auto scissor = reinterpret_cast<Scissor>(SDL_GL_GetProcAddress("glScissor"));
    if (!clearColor || !clear || !enable || !scissor) {
        out << "the OpenGL functions could not be loaded\n";
        SDL_GL_DeleteContext(context);
        return;
    }
    int alpha = 0;
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha);
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window.get(), &w, &h);
    const frontend::ProbeLayout layout = frontend::probeLayout(w, h, 40);
    constexpr unsigned kColorBuffer = 0x4000, kScissorTest = 0x0C11; // GL_COLOR_BUFFER_BIT, GL_SCISSOR_TEST
    showFor(probeMilliseconds(), [&] {
        enable(kScissorTest);
        scissor(0, 0, w, h);
        clearColor(0, 0, 0, 0);
        clear(kColorBuffer);
        scissor(layout.red.x, layout.red.y, layout.red.w, layout.red.h);
        clearColor(0.8f * 0.47f, 0, 0, 0.47f); // Premultiplied: what compositors expect.
        clear(kColorBuffer);
        scissor(layout.box.x, layout.box.y, layout.box.w, layout.box.h);
        clearColor(0.12f, 0.13f, 0.16f, 1);
        clear(kColorBuffer);
        SDL_GL_SwapWindow(window.get());
    });
    out << "context ok, alpha bits granted " << alpha << ", margin cleared to alpha 0, red margin alpha 0.47\n";
    SDL_GL_DeleteContext(context);
}

// What SDL hands out for setting the window geometry (Wayland) or _GTK_FRAME_EXTENTS (X11).
// Only looked at: using the handles needs libwayland-client or libX11, which are not linked.
inline void probeWindowManagerHandles(std::ostream &out) {
    out << "  window system handles from SDL_GetWindowWMInfo: ";
    ProbeWindow window = openProbeWindow(0);
    if (!window) {
        out << "no window: " << SDL_GetError() << '\n';
        return;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window.get(), &info)) {
        out << "not available: " << SDL_GetError() << '\n';
        return;
    }
    switch (info.subsystem) {
    case SDL_SYSWM_WAYLAND:
        out << "Wayland, display " << (info.info.wl.display ? "yes" : "no") << ", surface "
            << (info.info.wl.surface ? "yes" : "no") << ", xdg_surface " << (info.info.wl.xdg_surface ? "yes" : "no")
            << ", xdg_toplevel " << (info.info.wl.xdg_toplevel ? "yes" : "no") << '\n'
            << "    xdg_surface_set_window_geometry needs libwayland-client and the xdg-shell protocol code: "
               "not linked, so not tried. STOPPED: asking the owner.\n";
        break;
    case SDL_SYSWM_X11:
        out << "X11, display " << (info.info.x11.display ? "yes" : "no") << ", window "
            << (info.info.x11.window ? "yes" : "no") << '\n'
            << "    _GTK_FRAME_EXTENTS needs XChangeProperty from libX11 and a 32 bit ARGB visual: not linked, so "
               "not tried. STOPPED: asking the owner.\n";
        break;
    default:
        out << "another system (subsystem " << int(info.subsystem) << "): nothing to try\n";
        break;
    }
}

void probeTransparency(std::ostream &out) {
    out << "can the window be transparent? Each window below is open for about two seconds: a half-transparent\n"
           "red margin round a dark box, and a fully transparent rim outside the red. Do you see the desktop\n"
           "through the red margin and through the rim? (X11 needs a compositor; try SDL_VIDEODRIVER=x11 too.)\n"
        << std::flush;
    probeRenderer(out, "1. SDL_Renderer, software", SDL_RENDERER_SOFTWARE);
    out << std::flush;
    probeRenderer(out, "2. SDL_Renderer, accelerated", SDL_RENDERER_ACCELERATED);
    out << std::flush;
    probeOpenGl(out);
    out << std::flush;
    probeWindowManagerHandles(out);
}

} // namespace borderless
