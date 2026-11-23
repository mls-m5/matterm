#include "mls-unit-test/unittest.h"
#include "tabstrip.h"
#include "themes.h"
#include "windowhit.h"

#include <cmath>
#include <set>

using namespace frontend;
using namespace frontend::settings;
using namespace frontend::themes;

namespace {
double luma(Rgb8 c) { // Brightness as a person sees it, 0..255 (Rec. 709
                      // weights on the stored values).
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}
double relative(Rgb8 c) { // WCAG relative luminance.
    const auto lin = [](unsigned char v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}
double contrast(Rgb8 a, Rgb8 b) {
    const double x = relative(a), y = relative(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
double distance(Rgb8 a, Rgb8 b) {
    return std::sqrt(double((a.r - b.r) * (a.r - b.r) +
                            (a.g - b.g) * (a.g - b.g) +
                            (a.b - b.b) * (a.b - b.b)));
}
} // namespace

TEST_SUIT_BEGIN(Themes)

TEST_CASE("The themes are the ones the settings name, grey first, each with a "
          "label") {
    EXPECT_EQ(allThemes().size(), themeIds().size());
    for (std::size_t i = 0; i < themeIds().size(); ++i) {
        EXPECT_EQ(allThemes()[i].id, themeIds()[i]);
        EXPECT(!allThemes()[i].label.empty());
    }
    EXPECT_EQ(allThemes()[0].id, std::string("grey"));
    EXPECT_EQ(theme("nope").id, std::string("grey")); // Unknown: today's look.
}

TEST_CASE("Grey is today's look exactly") {
    const Palette today;
    const auto strip = stripPalette(theme("grey"));
    EXPECT(tabstrip_detail::colors(strip).stripBg.r ==
           tabstrip_detail::colors(today).stripBg.r);
    EXPECT(tabstrip_detail::colors(strip).stripBg.g ==
           tabstrip_detail::colors(today).stripBg.g);
    EXPECT(tabstrip_detail::colors(strip).stripBg.b ==
           tabstrip_detail::colors(today).stripBg.b);
    EXPECT(tabstrip_detail::colors(strip).tabBg.r ==
           tabstrip_detail::colors(today).tabBg.r);
    const auto bg = terminalBackground(theme("grey"));
    EXPECT(bg.r == today.background.r && bg.g == today.background.g &&
           bg.b == today.background.b);
}

TEST_CASE("The strip of a theme is the colour it names, and the border is the "
          "strip's colour") {
    for (const auto &t : allThemes()) {
        const auto palette = stripPalette(t);
        const auto colors = tabstrip_detail::colors(palette);
        const auto expected = t.id == "grey"
                                  ? tabstrip_detail::colors(Palette()).stripBg
                                  : t.strip;
        EXPECT(colors.stripBg.r == expected.r &&
               colors.stripBg.g == expected.g &&
               colors.stripBg.b == expected.b);
        const auto border = borderColor(palette);
        EXPECT(border.r == colors.stripBg.r && border.g == colors.stripBg.g &&
               border.b == colors.stripBg.b);
    }
}

TEST_CASE("The themes differ in hue only: about the brightness of grey, hues "
          "well apart") {
    const auto hue = [](Rgb8 c) {
        const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
        const double hi = std::max({r, g, b}), lo = std::min({r, g, b}),
                     d = hi - lo;
        if (d == 0) {
            return 0.0;
        }
        double h = hi == r   ? std::fmod((g - b) / d, 6.0)
                   : hi == g ? (b - r) / d + 2
                             : (r - g) / d + 4;
        h *= 60;
        return h < 0 ? h + 360 : h;
    };
    const auto apart = [&](double a, double b) {
        const double d = std::fabs(a - b);
        return std::min(d, 360 - d);
    };
    for (std::size_t i = 0; i < allThemes().size(); ++i) {
        const auto &a = allThemes()[i];
        EXPECT(std::fabs(luma(a.strip) - luma(allThemes()[0].strip)) <=
               8.0); // Brightness is grey's.
        for (std::size_t j = i + 1; j < allThemes().size(); ++j) {
            const auto &b = allThemes()[j];
            EXPECT(distance(a.strip, b.strip) >= 25.0);
            if (i > 0) { // Grey has no hue.
                EXPECT(apart(hue(a.strip), hue(b.strip)) >= 30.0);
            }
        }
    }
}

TEST_CASE("The look stays calm and readable") {
    for (const auto &t : allThemes()) {
        const auto palette = stripPalette(t);
        const auto colors = tabstrip_detail::colors(palette);
        EXPECT(luma(t.strip) <= 60.0);                       // Dark.
        EXPECT(contrast(colors.tabFg, colors.tabBg) >= 4.5); // Tab text.
        EXPECT(contrast(colors.dim, colors.stripBg) >=
               3.0); // Buttons and dim text.
        EXPECT(contrast(colors.activeFg, colors.activeBg) >= 7.0);
        const auto terminal = terminalBackground(t);
        const Palette base;
        EXPECT(contrast(base.foreground, terminal) >=
               10.0); // Programs' text keeps its contrast.
        EXPECT(std::abs(int(terminal.r) - int(base.background.r)) <=
               10); // A slight tint only.
        EXPECT(std::abs(int(terminal.g) - int(base.background.g)) <= 10);
        EXPECT(std::abs(int(terminal.b) - int(base.background.b)) <= 10);
    }
}

TEST_CASE("Every non-grey theme tints the terminal background a little, "
          "differently") {
    std::set<std::tuple<int, int, int>> seen;
    for (const auto &t : allThemes()) {
        const auto bg = terminalBackground(t);
        seen.insert({bg.r, bg.g, bg.b});
    }
    EXPECT_EQ(seen.size(), allThemes().size());
}

TEST_CASE("The local computer has the user's theme; a remote one a theme of "
          "its own") {
    Settings s;
    EXPECT_EQ(themeIdFor(s, ""), std::string("grey"));
    std::string error;
    EXPECT(s.set("theme", "blue", error));
    EXPECT_EQ(themeIdFor(s, ""), std::string("blue"));
}

TEST_CASE("The automatic choice is fixed by the name, ignores case, and is "
          "never the local theme") {
    Settings s;
    const auto first = themeIdFor(s, "server");
    EXPECT(first != "grey");
    EXPECT_EQ(themeIdFor(s, "server"), first);
    EXPECT_EQ(themeIdFor(s, "SERVER"), first);
    EXPECT_EQ(themeIdFor(s, " server "), first);
    std::set<std::string> reached;
    for (int i = 0; i < 200; ++i) {
        const auto id = themeIdFor(s, "host-" + std::to_string(i));
        EXPECT(id != "grey");
        reached.insert(id);
    }
    EXPECT_EQ(reached.size(),
              std::size_t(4)); // Blue, green, red, amber all get used.
    std::string error;
    EXPECT(s.set("theme",
                 "green",
                 error)); // The user's own theme is left to the local computer.
    for (int i = 0; i < 200; ++i) {
        EXPECT(themeIdFor(s, "host-" + std::to_string(i)) != "green");
    }
}

TEST_CASE("An override wins; auto goes back to the automatic choice; a bad one "
          "is ignored") {
    Settings s;
    std::string error;
    const auto automatic = themeIdFor(s, "server");
    const std::string other = automatic == "red" ? "amber" : "red";
    EXPECT(s.setComputer("server", other, error));
    EXPECT_EQ(themeIdFor(s, "server"), other);
    EXPECT_EQ(themeIdFor(s, "other box"), themeIdFor(Settings(), "other box"));
    EXPECT(s.setComputer("server", "auto", error));
    EXPECT_EQ(themeIdFor(s, "server"), automatic);
    const auto loaded =
        Settings::fromText(R"({"computers":{"server":"plaid"}})");
    EXPECT_EQ(themeIdFor(loaded.settings, "server"), automatic);
    EXPECT(
        s.setComputer("box", "grey", error)); // An override may even be grey.
    EXPECT_EQ(themeIdFor(s, "box"), std::string("grey"));
}

TEST_SUIT_END
