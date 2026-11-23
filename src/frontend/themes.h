#pragma once

// The themes (docs/tasks/settings.md): dark only. A theme colours the frame
// (the tab strip, its marks and the window border, which is the strip's colour)
// and tints the terminal's default background slightly; the programs' own
// colours are never touched. The themes differ in hue only (about the
// brightness of grey, the look of today), calm and readable (checked by test).
// Colour is not the only sign of where a window is connected: the computer's
// name is written in the strip's gap (placeComputerName in tabstrip.h).
//
// A theme is defined by the colour its strip should have; stripPalette() puts
// it in Palette::frame, which tabstrip's colors() and so borderColor() read
// (grey leaves it unset: exactly today's derivation).
//
// The theme of a computer: the local one has the user's theme (default grey); a
// computer reached with
// --daemon-host has its override from the settings, else an automatic choice
// from its name (the same name always gets the same theme, and never the local
// theme, so a remote window never looks like the local one).
#include "palette.h"
#include "settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace frontend::themes {

struct Theme {
    std::string id;
    std::string label;
    Rgb8 strip;      // The tab strip's background (and so the border).
    Rgb8 tint;       // Mixed into the terminal's default background...
    int tintPercent; // ...this much (0: none).
};

inline const std::vector<Theme> &allThemes() {
    // The order is the order of settings::themeIds(). Grey is what the window
    // always looked like.
    static const std::vector<Theme> all = {
        {"grey", "Grey", {46, 46, 46}, {0, 0, 0}, 0},
        {"blue", "Blue", {30, 46, 88}, {40, 80, 200}, 5},
        {"green", "Green", {22, 56, 30}, {60, 180, 90}, 5},
        {"red", "Red", {94, 33, 37}, {220, 60, 60}, 5},
        {"amber", "Amber", {62, 44, 12}, {220, 160, 40}, 5},
    };
    return all;
}

// Unknown id: grey.
inline const Theme &theme(const std::string &id) {
    for (const auto &t : allThemes()) {
        if (t.id == id) {
            return t;
        }
    }
    return allThemes().front();
}

namespace detail {
inline std::string lower(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return text;
}
inline std::string trimmed(const std::string &text) {
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}
inline std::uint64_t fnv1a(const std::string &text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    return h;
}
} // namespace detail

// The palette for the tab strip and the border of a theme.
inline Palette stripPalette(const Theme &t) {
    Palette p;
    if (t.id != "grey") {
        p.frame = t.strip;
    }
    return p;
}

// The default background of terminals: today's, tinted.
inline Rgb8 terminalBackground(const Theme &t) {
    const Palette base;
    const auto mix = [&](unsigned char a, unsigned char b) {
        return static_cast<unsigned char>(
            (a * (100 - t.tintPercent) + b * t.tintPercent) / 100);
    };
    return {mix(base.background.r, t.tint.r),
            mix(base.background.g, t.tint.g),
            mix(base.background.b, t.tint.b)};
}

// The theme id for the computer a window is connected to; `computer` is empty
// for the local one.
inline std::string themeIdFor(const settings::Settings &s,
                              const std::string &computer) {
    const std::string own = s.choice("theme");
    const std::string name = detail::lower(detail::trimmed(computer));
    if (name.empty()) {
        return own;
    }
    const std::string chosen = s.computerChoice(computer);
    if (chosen != "auto") {
        return chosen;
    }
    // Automatic: by the name, among the themes other than the local one (in the
    // fixed order of the list).
    std::vector<std::string> candidates;
    for (const auto &id : settings::themeIds()) {
        if (id != own && id != "grey") {
            candidates.push_back(id);
        }
    }
    if (own != "grey") {
        candidates.push_back("grey"); // Another theme is the local one: grey is
                                      // free for a remote computer.
    }
    return candidates[detail::fnv1a(name) % candidates.size()];
}

} // namespace frontend::themes
