#pragma once

// Does this window have a system frame at all? On GNOME Wayland SDL draws it with libdecor, and
// libdecor without a plugin ("No plugins found, falling back on no decorations") leaves the window
// with no frame whatever. Several ways of telling are tried side by side until the owner has seen
// which one is right on his machines (docs/TABS_PLAN.md section 16, "Finding out that there is no
// frame"). Pure: the SDL and file system side fills in a FrameProbe; nothing here touches either.
#include <string>

namespace frontend {

struct FrameProbe {
    std::string videoDriver;     // SDL_GetCurrentVideoDriver(), e.g. "wayland", "x11"
    bool bordersKnown = false;   // SDL_GetWindowBordersSize() succeeded
    int top = 0, left = 0, bottom = 0, right = 0;
    int pluginFiles = -1;        // libdecor plugin libraries found; -1: no plugin folder was found to look in
    bool libdecorSaidNoPlugins = false; // "No plugins found" was printed to stderr while SDL started
};

enum class FrameState { HasFrame, NoFrame, Unknown };

inline bool isWayland(const FrameProbe &probe) {
    return probe.videoDriver == "wayland";
}

// 1. The size of the borders SDL reports for a window that was created with a frame. Zero on every
// side, or a title bar of zero, is a window without one. Only believed on Wayland: elsewhere a window
// manager (or the lack of one) decides and zero says little.
inline FrameState detectByBorders(const FrameProbe &probe) {
    if (!isWayland(probe) || !probe.bordersKnown) {
        return FrameState::Unknown;
    }
    return probe.top > 0 || probe.left > 0 || probe.bottom > 0 || probe.right > 0 ? FrameState::HasFrame
                                                                                      : FrameState::NoFrame;
}

// 2. libdecor needs a plugin library (gtk or cairo) in its plugin folder; a folder without one is a
// window without a frame. A missing folder is not an answer: the library may live elsewhere.
inline FrameState detectByPlugins(const FrameProbe &probe) {
    if (!isWayland(probe) || probe.pluginFiles < 0) {
        return FrameState::Unknown;
    }
    return probe.pluginFiles > 0 ? FrameState::HasFrame : FrameState::NoFrame;
}

// 3. libdecor says so on stderr when SDL starts. Silence is not proof (another version may word it
// differently), so it only ever answers "no frame".
inline FrameState detectByMessage(const FrameProbe &probe) {
    return isWayland(probe) && probe.libdecorSaidNoPlugins ? FrameState::NoFrame : FrameState::Unknown;
}

// Which of them decides (MATTERM_FRAME_DETECT while they are compared; later a setting).
enum class DetectVariant { Off, Borders, Plugins, Message, Any };

inline bool parseDetectVariant(const std::string &text, DetectVariant &out) {
    static const struct {
        const char *name;
        DetectVariant variant;
    } names[] = {{"off", DetectVariant::Off},         {"borders", DetectVariant::Borders},
                 {"plugins", DetectVariant::Plugins}, {"message", DetectVariant::Message},
                 {"any", DetectVariant::Any}};
    for (const auto &entry : names) {
        if (text == entry.name) {
            out = entry.variant;
            return true;
        }
    }
    return false;
}

// `any`: no frame if any of the three says so and none says there is one.
inline FrameState decide(DetectVariant variant, const FrameProbe &probe) {
    switch (variant) {
    case DetectVariant::Off: return FrameState::Unknown;
    case DetectVariant::Borders: return detectByBorders(probe);
    case DetectVariant::Plugins: return detectByPlugins(probe);
    case DetectVariant::Message: return detectByMessage(probe);
    case DetectVariant::Any: break;
    }
    const FrameState all[] = {detectByBorders(probe), detectByPlugins(probe), detectByMessage(probe)};
    bool none = false;
    for (const FrameState state : all) {
        if (state == FrameState::HasFrame) return FrameState::HasFrame;
        none = none || state == FrameState::NoFrame;
    }
    return none ? FrameState::NoFrame : FrameState::Unknown;
}

inline const char *detectVariantName(DetectVariant variant) {
    switch (variant) {
    case DetectVariant::Off: return "off";
    case DetectVariant::Borders: return "borders";
    case DetectVariant::Plugins: return "plugins";
    case DetectVariant::Message: return "message";
    case DetectVariant::Any: break;
    }
    return "any";
}

inline const char *frameStateName(FrameState state) {
    switch (state) {
    case FrameState::HasFrame: return "has a frame";
    case FrameState::NoFrame: return "no frame";
    case FrameState::Unknown: break;
    }
    return "cannot tell";
}

// The line for the log and the first terminal when the own frame replaces a missing one.
inline std::string noFrameNote(const std::string &variantName) {
    return "matterm: the system draws no window frame here (found by '" + variantName +
           "'); using the built-in frame (MATTERM_FRAME_DETECT=off turns this off)";
}

} // namespace frontend
