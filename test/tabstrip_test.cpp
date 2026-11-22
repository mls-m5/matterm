#include "mls-unit-test/unittest.h"
#include "tabstrip.h"

#include <string>
#include <vector>

namespace {
using namespace frontend;
using Kind = StripHit::Kind;

std::vector<TabInfo> makeTabs(int count,
                              int active,
                              const std::string &title = "shell") {
    std::vector<TabInfo> tabs;
    for (int i = 0; i < count; ++i) {
        TabInfo t;
        t.id = TabId(i + 1);
        t.title = title + std::to_string(i);
        t.active = i == active;
        tabs.push_back(t);
    }
    return tabs;
}

std::string text(const TabStrip &strip) {
    std::string out;
    for (const auto &c : strip.cells) {
        out += c.codePoint < 128 ? char(c.codePoint) : '?';
    }
    return out;
}

std::string utf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += char(cp);
    }
    else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    return out;
}
// Characters that draw nothing or change how neighbours are ordered or joined.
bool invisible(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0x00ad || cp == 0x061c ||
           cp == 0x180e || (cp >= 0x200b && cp <= 0x200f) ||
           (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff ||
           (cp >= 0xfff9 && cp <= 0xfffb) || (cp >= 0xe0000 && cp <= 0xe007f);
}

// Whatever the input, these always hold.
bool sound(const TabStrip &strip, int columns, std::size_t tabs) {
    if (strip.cells.size() != std::size_t(std::max(0, columns))) {
        return false;
    }
    int lastEnd = 0;
    for (const auto &r : strip.regions) {
        if (r.begin < lastEnd || r.end <= r.begin || r.end > columns) {
            return false; // Overlapping, empty or outside.
        }
        lastEnd = r.end;
        if ((r.hit.kind == Kind::Tab || r.hit.kind == Kind::Close) &&
            (r.hit.tab < 0 || std::size_t(r.hit.tab) >= tabs)) {
            return false;
        }
    }
    // A wide character is always followed by its blank continuation.
    for (std::size_t i = 0; i < strip.cells.size(); ++i) {
        if (displayWidth(strip.cells[i].codePoint) == 2 &&
            (i + 1 >= strip.cells.size() ||
             strip.cells[i + 1].codePoint != U' ')) {
            return false;
        }
    }
    // hit() agrees with the regions everywhere.
    for (int c = -2; c < columns + 2; ++c) {
        bool inside = false;
        for (const auto &r : strip.regions) {
            if (c >= r.begin && c < r.end) {
                inside = strip.hit(c) == r.hit;
            }
        }
        if (!inside && strip.hit(c).kind != Kind::None) {
            return false;
        }
    }
    return true;
}

bool hasTab(const TabStrip &strip, int tab) {
    for (const auto &r : strip.regions) {
        if (r.hit.kind == Kind::Tab && r.hit.tab == tab) {
            return true;
        }
    }
    return false;
}
bool hasKind(const TabStrip &strip, Kind kind) {
    for (const auto &r : strip.regions) {
        if (r.hit.kind == kind) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_SUIT_BEGIN(TabStrip)

TEST_CASE("Widths of characters") {
    EXPECT_EQ(displayWidth(U'a'), 1);
    EXPECT_EQ(displayWidth(U'å'), 1);
    EXPECT_EQ(displayWidth(U'日'), 2); // CJK
    EXPECT_EQ(displayWidth(U'Ａ'), 2); // Fullwidth A
    EXPECT_EQ(displayWidth(U'\U0001f600'), 2);
    EXPECT_EQ(displayWidth(U'́'), 0); // Combining
    EXPECT_EQ(displayWidth(U'​'), 0);
    EXPECT_EQ(displayWidth(U'\n'), 0);
}

TEST_CASE("No tabs, one tab, a few tabs: titles, new-tab button, hit regions") {
    const auto none = buildTabStrip({}, 40);
    EXPECT(sound(none, 40, 0));
    EXPECT(hasKind(none, Kind::NewTab));
    EXPECT_EQ(none.visibleCount, 0);
    EXPECT(none.hit(1).kind == Kind::NewTab);
    EXPECT(none.hit(30).kind == Kind::None);

    auto one = makeTabs(1, 0, "main");
    const auto s1 = buildTabStrip(one, 40);
    EXPECT(sound(s1, 40, 1));
    EXPECT(text(s1).find("main0") != std::string::npos);
    EXPECT(s1.mode == StripMode::Titles);
    EXPECT(s1.hit(2).kind == Kind::Tab);
    EXPECT_EQ(s1.hit(2).tab, 0);
    // Close button right after the title, then the new-tab button.
    const auto close = text(s1).find("main0") + 5 + 1;
    EXPECT(s1.hit(int(close)).kind == Kind::Close);
    EXPECT_EQ(s1.hit(int(close)).tab, 0);
    EXPECT(s1.hit(int(close) + 2).kind == Kind::NewTab);

    const auto three = makeTabs(3, 1);
    const auto s3 = buildTabStrip(three, 80);
    EXPECT(sound(s3, 80, 3));
    EXPECT(s3.mode == StripMode::Titles);
    EXPECT_EQ(s3.visibleCount, 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT(hasTab(s3, i));
    }
}

TEST_CASE("The active tab and the activity mark are visible") {
    auto tabs = makeTabs(3, 1);
    tabs[2].activity = true;
    Palette palette;
    const auto strip = buildTabStrip(tabs, 80, palette);
    auto firstCell = [&](int tab) -> const FChar & {
        for (const auto &r : strip.regions) {
            if (r.hit.kind == Kind::Tab && r.hit.tab == tab) {
                return strip.cells[std::size_t(r.begin)];
            }
        }
        return strip.cells[0];
    };
    const auto &active = firstCell(1);
    const auto &other = firstCell(0);
    EXPECT(active.background.r == palette.foreground.r); // Inverted.
    EXPECT(other.background.r != active.background.r);
    bool marked = false;
    for (const auto &c : strip.cells) {
        marked = marked || c.codePoint == U'●';
    }
    EXPECT(marked);
    auto quiet = makeTabs(3, 1);
    const auto none = buildTabStrip(quiet, 80, palette);
    for (const auto &c : none.cells) {
        EXPECT(c.codePoint != U'●');
    }
}

TEST_CASE("Titles are cut by display width, never in the middle of a "
          "character") {
    auto tabs = makeTabs(1, 0, "");
    tabs[0].title =
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC"
        "\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // 12 wide chars
    for (int columns = 1; columns <= 60; ++columns) {
        const auto strip = buildTabStrip(tabs, columns);
        EXPECT(sound(strip, columns, 1));
    }
    const auto strip = buildTabStrip(tabs, 60);
    int wideCells = 0;
    for (const auto &c : strip.cells) {
        wideCells += displayWidth(c.codePoint) == 2;
    }
    EXPECT(wideCells > 0 && wideCells <= 9); // At most kMaxTitle columns.
    // Invalid UTF-8 and controls are harmless.
    tabs[0].title = std::string("a\xff\x01\xe6\x97 b\x1b[31m");
    EXPECT(sound(buildTabStrip(tabs, 30), 30, 1));
    tabs[0].title = "";
    EXPECT(text(buildTabStrip(tabs, 30)).find("Tab 1") != std::string::npos);
}


TEST_CASE("A title set by a program cannot put invisible or reordering characters "
          "into the strip") {
    const char32_t hostile[] = {0x00ad, 0x061c, 0x180e, 0x200b, 0x200d, 0x200e, 0x200f,
                                0x2028, 0x2029, 0x202a, 0x202b, 0x202c, 0x202d, 0x202e,
                                0x2060, 0x2066, 0x2067, 0x2068, 0x2069, 0x206a, 0xfe0f,
                                0xfeff, 0xfff9, 0xfffa, 0xfffb, 0xe0001, 0xe0041, 0xe007f,
                                0x1b,   0x07,   0x00,   0x7f,   0x85,   0x9b};
    for (const char32_t cp : hostile) {
        auto tabs = makeTabs(1, 0, "");
        tabs[0].title = "ab" + utf8(cp) + "cd" + utf8(cp) + utf8(cp) + "ef";
        const auto strip = buildTabStrip(tabs, 40);
        EXPECT(sound(strip, 40, 1));
        for (const auto &c : strip.cells) {
            EXPECT(!invisible(c.codePoint));
        }
        // The visible letters stay together: the title reads "abcdef".
        EXPECT(text(strip).find("abcdef") != std::string::npos);
    }
    // A very long hostile title (a program may send 2 KiB) is cut like any other.
    auto tabs = makeTabs(3, 1, "");
    std::string long2k;
    while (long2k.size() < 2048) {
        long2k += utf8(0x202e) + "x" + utf8(0x1f600);
    }
    for (auto &t : tabs) {
        t.title = long2k;
    }
    for (int columns = 0; columns <= 120; ++columns) {
        const auto strip = buildTabStrip(tabs, columns);
        EXPECT(sound(strip, columns, 3));
        for (const auto &c : strip.cells) {
            EXPECT(!invisible(c.codePoint));
        }
    }
}

TEST_CASE("Overflow: shrink titles, then numbers, then a scrolled window") {
    const auto some = makeTabs(5, 0, "a-long-title-");
    const auto wide = buildTabStrip(some, 200);
    EXPECT(wide.mode == StripMode::Titles);
    const auto shrunk = buildTabStrip(some, 80);
    EXPECT(sound(shrunk, 80, 5));
    EXPECT(shrunk.mode == StripMode::Titles);
    EXPECT(shrunk.visibleCount == 5);
    bool ellipsis = false;
    for (const auto &c : shrunk.cells) {
        ellipsis = ellipsis || c.codePoint == U'…';
    }
    EXPECT(ellipsis);
    const auto many = makeTabs(12, 3);
    const auto numbers = buildTabStrip(many, 70);
    EXPECT(sound(numbers, 70, 12));
    EXPECT(numbers.mode == StripMode::Numbers);
    EXPECT_EQ(numbers.visibleCount, 12);
    EXPECT(text(numbers).find(" 12 ") != std::string::npos);
    // Only the active tab has a close button in numbers mode.
    int closes = 0;
    for (const auto &r : numbers.regions) {
        closes += r.hit.kind == Kind::Close;
    }
    EXPECT_EQ(closes, 1);
    const auto fifty = makeTabs(50, 30);
    const auto scrolled = buildTabStrip(fifty, 60);
    EXPECT(sound(scrolled, 60, 50));
    EXPECT(scrolled.mode == StripMode::Scrolled);
    EXPECT(hasKind(scrolled, Kind::ScrollLeft));
    EXPECT(hasKind(scrolled, Kind::ScrollRight));
    EXPECT(hasTab(scrolled, 30)); // The active tab is always shown.
    EXPECT(scrolled.visibleCount < 50);
    EXPECT(scrolled.firstVisible > 0);
    // The owner may keep a scroll position: it holds while the active tab is in
    // view and is moved when it is not.
    const auto kept =
        buildTabStrip(fifty, 60, Palette(), scrolled.firstVisible);
    EXPECT_EQ(kept.firstVisible, scrolled.firstVisible);
    auto first = makeTabs(50, 0);
    const auto atStart = buildTabStrip(first, 60);
    EXPECT_EQ(atStart.firstVisible, 0);
    const auto scrolledAway = buildTabStrip(first, 60, Palette(), 20);
    EXPECT_EQ(scrolledAway.firstVisible, 0); // Active tab 0 forces it back.
    auto last = makeTabs(50, 49);
    const auto atEnd = buildTabStrip(last, 60);
    EXPECT(hasTab(atEnd, 49));
    EXPECT(atEnd.firstVisible + atEnd.visibleCount == 50);
}

TEST_CASE("Every width from 0 to 200 columns, many tab counts, stays sound") {
    for (const int tabCount : {0, 1, 2, 3, 7, 10, 50}) {
        for (const std::string &title :
             {std::string("t"),
              std::string("a rather long title indeed"),
              std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E x")}) {
            for (int columns = 0; columns <= 200; ++columns) {
                for (const int active : {0, tabCount / 2, tabCount - 1}) {
                    auto tabs =
                        makeTabs(tabCount, tabCount ? active : -1, title);
                    if (!tabs.empty()) {
                        tabs.back().activity = true;
                    }
                    const auto strip = buildTabStrip(tabs, columns);
                    EXPECT(sound(strip, columns, tabs.size()));
                    // With room for anything useful the active tab shows.
                    if (columns >= 16 && tabCount > 0) {
                        EXPECT(hasTab(strip, active));
                    }
                    // The new-tab button is there whenever it can be.
                    if (columns >= 40 && tabCount <= 3) {
                        EXPECT(hasKind(strip, Kind::NewTab));
                    }
                }
            }
        }
    }
}

TEST_CASE("A palette button closes the row when asked, at any width") {
    for (int columns = 0; columns <= 200; ++columns) {
        for (const int tabs : {0, 1, 3, 50}) {
            const auto strip = buildTabStrip(makeTabs(tabs, 0), columns, Palette(), 0, true);
            EXPECT(sound(strip, columns, std::size_t(tabs)));
            const bool button = !strip.regions.empty() &&
                                strip.regions.back().hit.kind == Kind::Palette;
            EXPECT(button == (columns >= 15));
            if (button) {
                EXPECT_EQ(strip.regions.back().end, columns);
                EXPECT_EQ(strip.regions.back().begin, columns - 3);
                EXPECT(strip.cells[std::size_t(columns - 2)].codePoint == U'\x2261');
                EXPECT(strip.hit(columns - 1).kind == Kind::Palette);
            }
        }
    }
    // Off by default: nothing changes.
    EXPECT(buildTabStrip(makeTabs(3, 0), 80).regions.back().hit.kind != Kind::Palette);
}

TEST_CASE("A share button sits before the palette button: grey when nothing is shared, red with the count") {
    for (int columns = 0; columns <= 200; ++columns) {
        for (const int tabs : {0, 1, 3, 50}) {
            for (const int shared : {0, 1, 12}) {
                const auto strip = buildTabStrip(makeTabs(tabs, 0), columns, Palette(), 0, true, ShareButton{true, shared});
                EXPECT(sound(strip, columns, std::size_t(tabs)));
                bool found = false;
                for (const auto &r : strip.regions) {
                    found = found || r.hit.kind == Kind::Share;
                }
                if (found) {
                    // Share, then the palette button, closing the row.
                    EXPECT(strip.regions.back().hit.kind == Kind::Palette);
                    const auto &share = strip.regions[strip.regions.size() - 2];
                    EXPECT(share.hit.kind == Kind::Share);
                    EXPECT_EQ(share.end, strip.regions.back().begin);
                    std::u32string text;
                    for (int c = share.begin; c < share.end; ++c) {
                        text.push_back(strip.cells[std::size_t(c)].codePoint);
                    }
                    EXPECT(text.find(kShareIcon) != std::u32string::npos); // The symbol, not the word.
                    EXPECT(text.find(U"Share") == std::u32string::npos);
                    EXPECT((text.find(U"12") != std::u32string::npos) == (shared == 12));
                    const auto bg = strip.cells[std::size_t(share.begin)].background;
                    EXPECT((bg.r > 150 && bg.g < 80) == (shared > 0)); // Red only when something is shared.
                }
                else {
                    EXPECT(columns < 12 + int(shareLabel(ShareButton{true, shared}).size()) + 3);
                }
            }
        }
    }
    // Without a share button nothing changes; without the palette button it ends the row.
    const auto only = buildTabStrip(makeTabs(2, 0), 80, Palette(), 0, false, ShareButton{true, 0});
    EXPECT(only.regions.back().hit.kind == Kind::Share);
    EXPECT_EQ(only.regions.back().end, 80);
    EXPECT(buildTabStrip(makeTabs(3, 0), 80).regions.back().hit.kind != Kind::Share);
}

TEST_CASE("Window buttons close the row: minimise, maximise or restore, close") {
    for (int columns = 0; columns <= 200; ++columns) {
        for (const int tabs : {0, 1, 3, 50}) {
            for (const bool maximized : {false, true}) {
                const auto strip = buildTabStrip(makeTabs(tabs, 0), columns, Palette(), 0, true, ShareButton{true, 1},
                                                 WindowButtons{true, maximized});
                EXPECT(sound(strip, columns, std::size_t(tabs)));
                const bool found = !strip.regions.empty() && strip.regions.back().hit.kind == Kind::CloseWindow;
                if (!found) {
                    for (const auto &r : strip.regions) {
                        EXPECT(r.hit.kind != Kind::Minimize && r.hit.kind != Kind::Maximize);
                    }
                    continue;
                }
                const std::size_t n = strip.regions.size();
                EXPECT(n >= 6);
                const auto &min = strip.regions[n - 3];
                const auto &max = strip.regions[n - 2];
                const auto &close = strip.regions[n - 1];
                EXPECT(min.hit.kind == Kind::Minimize && max.hit.kind == Kind::Maximize);
                EXPECT_EQ(min.end, max.begin);
                EXPECT_EQ(max.end, close.begin);
                EXPECT_EQ(close.end, columns);
                EXPECT_EQ(close.end - min.begin, 9);
                // Share and the palette button come before them.
                EXPECT(strip.regions[n - 4].hit.kind == Kind::Palette);
                EXPECT(strip.regions[n - 5].hit.kind == Kind::Share);
                auto glyph = [&](const StripRegion &r) { return strip.cells[std::size_t(r.begin + 1)].codePoint; };
                EXPECT(glyph(min) == kMinimizeIcon);
                EXPECT(glyph(max) == (maximized ? kRestoreIcon : kMaximizeIcon));
                EXPECT(glyph(close) == kCloseWindowIcon);
            }
        }
    }
    // The buttons alone (no Share, no palette) still end the row, and they are off by default.
    const auto alone = buildTabStrip(makeTabs(2, 0), 80, Palette(), 0, false, ShareButton{}, WindowButtons{true, false});
    EXPECT(alone.regions.back().hit.kind == Kind::CloseWindow);
    EXPECT_EQ(alone.regions.back().end, 80);
    for (const auto &r : buildTabStrip(makeTabs(2, 0), 80, Palette(), 0, true, ShareButton{true, 0}).regions) {
        EXPECT(r.hit.kind != Kind::CloseWindow);
    }
    // Too narrow: the tabs win and the buttons are dropped, whole.
    for (const auto &r : buildTabStrip(makeTabs(2, 0), 11, Palette(), 0, true, ShareButton{true, 0}, WindowButtons{true, false}).regions) {
        EXPECT(r.hit.kind != Kind::Minimize);
    }
}

TEST_CASE("The share button is a symbol; shared or not shows in the text, not only in the colour") {
    EXPECT(kShareIcon == U'\u2234'); // One constant to change if another symbol is preferred.
    const std::u32string none = shareLabel(ShareButton{true, 0});
    const std::u32string two = shareLabel(ShareButton{true, 2});
    const std::u32string many = shareLabel(ShareButton{true, 12});
    EXPECT(none == U" \u2234 ");
    EXPECT(two == U" \u2234 2 ");
    EXPECT(many == U" \u2234 12 ");
    // Nothing shared: no digit anywhere. Shared: the count is there. A colour-blind owner can tell.
    for (const char32_t c : none) EXPECT(c < U'0' || c > U'9');
    EXPECT(none != two && two != many);
    EXPECT(none.size() < two.size());
    // The symbol is a glyph of the strip's font list (the cmap test checks the font has it).
    bool listed = false;
    for (const char32_t g : kStripGlyphs) listed = listed || g == kShareIcon;
    EXPECT(listed);
    EXPECT_EQ(displayWidth(kShareIcon), 1);
}

TEST_SUIT_END
