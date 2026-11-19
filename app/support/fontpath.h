#pragma once

#include <string>

// Application convenience only, not part of the renderer's font ownership.
// Uses executable-relative assets, never the current working directory.
// Explicit paths preserve the existing CLI override and are validated by
// SDL_ttf.
std::string applicationFontPath(const char *explicitPath = nullptr);
