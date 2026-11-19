#include "fontpath.h"

#include <SDL.h>

#include <filesystem>
#include <memory>
#include <stdexcept>

std::string applicationFontPath(const char *explicitPath) {
    if (explicitPath && *explicitPath) {
        return explicitPath;
    }
#ifdef MATTERM_HAS_BUNDLED_FONT
    std::unique_ptr<char, decltype(&SDL_free)> base(SDL_GetBasePath(),
                                                    SDL_free);
    if (!base) {
        throw std::runtime_error(SDL_GetError());
    }
    const auto path = std::filesystem::path(base.get()) /
                      "assets/fonts/JetBrainsMono-Regular.ttf";
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Bundled font missing: " + path.string() +
                                 ". Rebuild/copy assets/fonts with the "
                                 "executable, or provide a font path.");
    }
    return path.string();
#else
    throw std::runtime_error(
        "No bundled font in this build. Pass /path/to/font.ttf, or enable "
        "MATTERM_BUNDLE_FONT.");
#endif
}
