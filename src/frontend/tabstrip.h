#pragma once

// The tab strip as one row of cells plus hit regions (docs/TABS_PLAN.md section
// 4, docs/tasks/tabs.md workstream C). Pure data in, cells out: no SDL, no
// terminal, no window. The strip is drawn by the same cell renderer as a
// terminal, so it looks the same natively and in the browser.
//
// One cell holds one code point, so a wide character is its own cell followed
// by a blank cell of the same style, and titles are cut by display width, never
// in the middle of a character.
//
// Overflow rule, in this order: titles at their natural width (at most
// kMaxTitle columns), titles shrunk (cut with an ellipsis) down to kMinTitle,
// then only numbers (the active tab keeps its close button), then a window of
// numbers with `<` and `>` scroll buttons that always shows the active tab.
#include "fchar.h"
#include "palette.h"
#include "tabpulse.h"
#include "tabtypes.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace frontend {

// Every non-ASCII glyph the strip draws (the font must have them; see
// test/chat_glyphs_test.cpp): activity mark, close button, ellipsis.
// The mark of a tab whose terminals are busy on their own (a fisheye: unlike the amber ● that says
// something happened while you were away).
inline constexpr char32_t kWorkingIcon = U'◉';
// The window buttons of a borderless window (docs/TABS_PLAN.md section 16).
inline constexpr char32_t kMinimizeIcon = U'–';
inline constexpr char32_t kMaximizeIcon = U'□';
inline constexpr char32_t kRestoreIcon = U'◫'; // U+25EB (the font has no overlapping-squares glyph)
inline constexpr char32_t kCloseWindowIcon = U'✕';
// The share button's symbol (an up arrow; the owner's choice). The bundled font has no
// box-with-arrow; U+2234 (three dots) and U+2197 are the other candidates it has.
inline constexpr char32_t kShareIcon = U'\u21E7';
inline constexpr char32_t kStripGlyphs[] = {kShareIcon, U'●', U'×', U'…', U'≡', kWorkingIcon,
                                            kMinimizeIcon, kMaximizeIcon, kRestoreIcon, kCloseWindowIcon};

inline constexpr int kMaxTitle = 20; // Columns of a title at most.
inline constexpr int kMinTitle = 4;  // Shrinking stops here.

struct StripHit {
    enum class Kind { None, Tab, Close, NewTab, ScrollLeft, ScrollRight, Palette, Share, Minimize, Maximize, CloseWindow };
    Kind kind = Kind::None;
    int tab = -1; // Index into the TabInfo list for Tab and Close.
    bool operator==(const StripHit &o) const {
        return kind == o.kind && tab == o.tab;
    }
};

enum class StripMode { Titles, Numbers, Scrolled };

// Columns [begin, end) belong to `hit`.
struct StripRegion {
    int begin = 0, end = 0;
    StripHit hit;
};

struct TabStrip {
    std::vector<FChar> cells;         // Exactly `columns` cells.
    std::vector<StripRegion> regions; // Disjoint, inside [0, columns).
    StripMode mode = StripMode::Titles;
    // The first tab shown and how many; the others are scrolled out of view.
    int firstVisible = 0;
    int visibleCount = 0;

    StripHit hit(int column) const {
        for (const auto &r : regions) {
            if (column >= r.begin && column < r.end) {
                return r.hit;
            }
        }
        return {};
    }
};

// Terminal columns of a code point: 0 for controls and zero-width marks, 2 for
// East Asian wide and fullwidth characters and emoji, else 1.
inline int displayWidth(char32_t cp) {
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) {
        return 0;
    }
    // Zero width, or invisible and able to reorder or join its neighbours (bidi
    // marks and overrides, line and paragraph separators, soft hyphen, tag characters):
    // a program sets the title, so none of these may reach the strip.
    if ((cp >= 0x0300 && cp <= 0x036f) || cp == 0x00ad || cp == 0x061c ||
        cp == 0x180e || (cp >= 0x200b && cp <= 0x200f) ||
        (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
        (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff ||
        (cp >= 0xfff9 && cp <= 0xfffb) || (cp >= 0xe0000 && cp <= 0xe007f)) {
        return 0;
    }
    const bool wide =
        (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) ||
        (cp >= 0x3041 && cp <= 0x33ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
        (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
        (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd);
    return wide ? 2 : 1;
}

namespace tabstrip_detail {

// Lenient UTF-8: bad bytes become U+FFFD; zero-width code points are dropped.
inline std::u32string decode(const std::string &text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        char32_t cp = 0xfffd;
        std::size_t n = 1;
        if (b < 0x80) {
            cp = b;
        }
        else if (b >= 0xc2 && b < 0xe0) {
            n = 2;
            cp = b & 0x1f;
        }
        else if (b >= 0xe0 && b < 0xf0) {
            n = 3;
            cp = b & 0x0f;
        }
        else if (b >= 0xf0 && b < 0xf5) {
            n = 4;
            cp = b & 0x07;
        }
        bool ok = n == 1 ? b < 0x80 : i + n <= text.size();
        for (std::size_t k = 1; ok && k < n; ++k) {
            const auto c = static_cast<unsigned char>(text[i + k]);
            ok = (c & 0xc0) == 0x80;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (!ok || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            cp = 0xfffd;
            n = 1;
        }
        i += n;
        if (displayWidth(cp) > 0) {
            out += cp;
        }
    }
    return out;
}

inline int widthOf(const std::u32string &s) {
    int w = 0;
    for (const char32_t cp : s) {
        w += displayWidth(cp);
    }
    return w;
}

// `s` cut to at most `cap` columns; an ellipsis marks the cut.
inline std::u32string cut(const std::u32string &s, int cap) {
    if (cap <= 0) {
        return {};
    }
    if (widthOf(s) <= cap) {
        return s;
    }
    std::u32string out;
    int w = 0;
    for (const char32_t cp : s) {
        const int cw = displayWidth(cp);
        if (w + cw > cap - 1) {
            break;
        }
        out += cp;
        w += cw;
    }
    out += U'…';
    return out;
}

struct Colors {
    Rgb8 stripBg, tabBg, tabFg, activeBg, activeFg, dim, mark, activeMark, working, activeWorking;
};

inline Rgb8 mix(Rgb8 a, Rgb8 b, int percentOfB) {
    auto m = [&](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(
            (x * (100 - percentOfB) + y * percentOfB) / 100);
    };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

inline Colors colors(const Palette &p) {
    Colors c;
    if (p.frame) { // A theme: the strip is its colour, the tabs a little
                   // lighter, the text a little brighter.
        c.stripBg = *p.frame;
        c.tabBg = mix(*p.frame, p.foreground, 12);
        c.tabFg = mix(p.foreground, p.background, 10);
    }
    else {
        c.stripBg = mix(p.background, p.foreground, 8);
        c.tabBg = mix(p.background, p.foreground, 18);
        c.tabFg = mix(p.foreground, p.background, 25);
    }
    c.activeBg = p.foreground;
    c.activeFg = p.background;
    c.dim = mix(p.foreground, p.background, p.frame ? 35 : 55);
    c.mark = {255, 190, 60};
    c.activeMark = {150, 90, 0};
    c.working = {90, 200, 120};
    c.activeWorking = {0, 120, 50};
    return c;
}

class Row {
public:
    Row(int columns, const Colors &c)
        : columns_(columns) {
        cells_.assign(std::size_t(std::max(0, columns)),
                      FChar{U' ', c.tabFg, c.stripBg});
    }
    int column() const {
        return at_;
    }
    int left() const {
        return columns_ - at_;
    }
    // A code point with its colors; a wide one also takes the next cell. It is
    // not placed if it does not fit.
    bool put(char32_t cp, Rgb8 fg, Rgb8 bg, CellAttribute attr = {}) {
        const int w = std::max(1, displayWidth(cp));
        if (at_ + w > columns_) {
            return false;
        }
        cells_[std::size_t(at_)] = FChar{cp, fg, bg, attr};
        if (w == 2) {
            cells_[std::size_t(at_) + 1] = FChar{U' ', fg, bg, attr};
        }
        at_ += w;
        return true;
    }
    void puts(const std::u32string &s,
              Rgb8 fg,
              Rgb8 bg,
              CellAttribute attr = {}) {
        for (const char32_t cp : s) {
            if (!put(cp, fg, bg, attr)) {
                break;
            }
        }
    }
    std::vector<FChar> take() {
        return std::move(cells_);
    }

private:
    int columns_;
    int at_ = 0;
    std::vector<FChar> cells_;
};

} // namespace tabstrip_detail

// The share button at the end of the strip (only offered when the panes can be shared, that is
// when they run in the local daemon): a grey share symbol when nothing in the active tab is shared,
// a red one followed by the count when N panes are. The difference is in the text (the count), not
// only in the colour, so it can be seen without telling grey from red.
struct ShareButton {
    bool shown = false;
    int shared = 0; // Panes of the active tab that are shared.
};

inline std::u32string shareLabel(const ShareButton &share) {
    std::u32string text = U" ";
    text.push_back(kShareIcon);
    text.push_back(U' ');
    if (share.shared > 0) {
        for (const char c : std::to_string(share.shared)) {
            text.push_back(char32_t(c));
        }
        text.push_back(U' ');
    }
    return text;
}

// The window buttons at the very end of the row of a borderless window: " – " " □ " " ✕ ".
struct WindowButtons {
    bool shown = false;
    bool maximized = false; // The middle button then restores (kRestoreIcon).
};

enum class NamePlacement { Center, Left, Right };
// "center", "left" or "right"; anything else: Center.
inline NamePlacement parseNamePlacement(const std::string &text) {
    return text == "left"    ? NamePlacement::Left
           : text == "right" ? NamePlacement::Right
                             : NamePlacement::Center;
}

// Where the name of the computer goes (and the one place to change when it
// moves): the gap of the strip, between the last tab (or its new-tab button)
// and the share, palette and window buttons: in the middle, at the left (next
// to the tabs) or at the right (against the buttons), as `placement` says (the
// setting computerNamePlacement). In a muted colour of the theme; shortened
// with an ellipsis to fit; nothing when there is no room for at least three
// characters. No click region is added: the gap is where the window is dragged.
inline void placeComputerName(TabStrip &strip,
                              const std::string &name,
                              const Palette &palette,
                              NamePlacement placement) {
    if (name.empty() || strip.cells.empty()) {
        return;
    }
    const int columns = int(strip.cells.size());
    int begin = 0, end = columns;
    for (const auto &r : strip.regions) {
        switch (r.hit.kind) {
        case StripHit::Kind::Tab:
        case StripHit::Kind::Close:
        case StripHit::Kind::NewTab:
        case StripHit::Kind::ScrollLeft:
        case StripHit::Kind::ScrollRight:
            begin = std::max(begin, r.end);
            break;
        default: // The buttons at the right.
            end = std::min(end, r.begin);
            break;
        }
    }
    constexpr int kMargin = 1; // A blank each side.
    const int room = end - begin - 2 * kMargin;
    if (room < 3) {
        return;
    }
    const auto text = tabstrip_detail::cut(tabstrip_detail::decode(name), room);
    const auto colours = tabstrip_detail::colors(palette);
    // A muted colour of the theme: its own colour lifted toward the text, so
    // that it reads on the strip.
    const Rgb8 muted =
        palette.frame
            ? tabstrip_detail::mix(*palette.frame, palette.foreground, 55)
            : colours.dim;
    const int leftmost = begin + kMargin,
              rightmost = end - kMargin - int(text.size());
    int at = placement == NamePlacement::Left ? leftmost
             : placement == NamePlacement::Right
                 ? rightmost
                 : leftmost + (rightmost - leftmost) / 2;
    for (const char32_t c : text) {
        strip.cells[std::size_t(at++)] = FChar{c, muted, colours.stripBg};
    }
}

inline TabStrip buildTabStrip(const std::vector<TabInfo> &tabs,
                              int columns,
                              const Palette &palette = Palette(),
                              int firstVisible = 0,
                              bool paletteButton = false,
                              ShareButton share = {},
                              WindowButtons window = {}) {
    using namespace tabstrip_detail;
    constexpr int kButtonWidth = 3;
    const std::u32string shareText = shareLabel(share);
    const int shareWidth = share.shown ? int(shareText.size()) : 0;
    const int paletteWidth = paletteButton ? kButtonWidth : 0;
    const int windowWidth = window.shown ? 3 * kButtonWidth : 0;
    if ((share.shown || paletteButton || window.shown) && columns >= 12 + shareWidth + paletteWidth + windowWidth) {
        // Buttons close the row after everything else: " Share ", " ≡ ", then the window buttons.
        TabStrip strip =
            buildTabStrip(tabs, columns - shareWidth - paletteWidth - windowWidth, palette, firstVisible, false);
        const Colors col = colors(palette);
        int at = columns - shareWidth - paletteWidth - windowWidth;
        if (share.shown) {
            const bool on = share.shared > 0;
            const Rgb8 fg = on ? Rgb8{255, 255, 255} : col.dim;
            const Rgb8 bg = on ? Rgb8{190, 35, 35} : col.tabBg;
            for (const char32_t c : shareText) {
                strip.cells.push_back(FChar{c, fg, bg});
            }
            strip.regions.push_back({at, at + shareWidth, {StripHit::Kind::Share, -1}});
            at += shareWidth;
        }
        if (paletteButton) {
            strip.cells.push_back(FChar{U' ', col.dim, col.stripBg});
            strip.cells.push_back(FChar{U'≡', col.dim, col.stripBg});
            strip.cells.push_back(FChar{U' ', col.dim, col.stripBg});
            strip.regions.push_back({at, at + kButtonWidth, {StripHit::Kind::Palette, -1}});
            at += kButtonWidth;
        }
        if (window.shown) {
            const struct {
                char32_t icon;
                StripHit::Kind kind;
            } buttons[] = {{kMinimizeIcon, StripHit::Kind::Minimize},
                           {window.maximized ? kRestoreIcon : kMaximizeIcon, StripHit::Kind::Maximize},
                           {kCloseWindowIcon, StripHit::Kind::CloseWindow}};
            for (const auto &button : buttons) {
                strip.cells.push_back(FChar{U' ', col.dim, col.stripBg});
                strip.cells.push_back(FChar{button.icon, col.dim, col.stripBg});
                strip.cells.push_back(FChar{U' ', col.dim, col.stripBg});
                strip.regions.push_back({at, at + kButtonWidth, {button.kind, -1}});
                at += kButtonWidth;
            }
        }
        return strip;
    }
    const Colors col = colors(palette);
    const int n = std::max(0, columns);
    const int count = int(tabs.size());
    TabStrip strip;
    Row row(n, col);
    auto region = [&](int begin, int end, StripHit hit) {
        // Regions stay disjoint and inside the row, whatever the width.
        begin = std::max(
            {0, begin, strip.regions.empty() ? 0 : strip.regions.back().end});
        end = std::min(n, end);
        if (begin < end) {
            strip.regions.push_back({begin, end, hit});
        }
    };

    std::vector<std::u32string> titles;
    titles.resize(std::size_t(count));
    for (int i = 0; i < count; ++i) {
        auto t = decode(tabs[std::size_t(i)].title);
        if (t.empty()) {
            const auto label = "Tab " + std::to_string(i + 1);
            t.assign(label.begin(), label.end());
        }
        titles[std::size_t(i)] = cut(t, kMaxTitle);
    }
    auto digits = [](int v) {
        int d = 1;
        while (v >= 10) {
            v /= 10;
            ++d;
        }
        return d;
    };
    auto number = [&](int i) {
        const auto text = std::to_string(i + 1);
        return std::u32string(text.begin(), text.end());
    };

    // Draws tab i; `label` is what is written between the marks.
    auto drawTab = [&](int i, const std::u32string &label, bool closeButton) {
        const auto &info = tabs[std::size_t(i)];
        Rgb8 bg = info.active ? col.activeBg : col.tabBg;
        if (info.pulseDark) bg = mix(bg, Rgb8{0, 0, 0}, TabPulse::kDarkenPercent);
        const Rgb8 fg = info.active ? col.activeFg : col.tabFg;
        const int begin = row.column();
        row.put(U' ', fg, bg);
        if (info.working) {
            row.put(kWorkingIcon, info.active ? col.activeWorking : col.working, bg);
        }
        else {
            const char32_t mark = info.activity ? U'●' : U' ';
            row.put(mark, info.active ? col.activeMark : col.mark, bg);
        }
        row.puts(label,
                 fg,
                 bg,
                 info.active ? CellAttribute::Bold : CellAttribute::None);
        row.put(U' ', fg, bg);
        const int closeAt = row.column();
        region(begin, closeAt, {StripHit::Kind::Tab, i});
        if (closeButton) {
            row.put(U'×', info.active ? fg : col.dim, bg);
            region(closeAt, row.column(), {StripHit::Kind::Close, i});
        }
    };
    auto drawGap = [&]() { row.put(U' ', col.tabFg, col.stripBg); };
    auto drawNew = [&]() {
        const int begin = row.column();
        row.put(U' ', col.dim, col.stripBg);
        row.put(U'+', col.dim, col.stripBg);
        row.put(U' ', col.dim, col.stripBg);
        region(begin, row.column(), {StripHit::Kind::NewTab, -1});
    };

    const int newWidth = 3;
    const bool room = n >= newWidth;
    const int avail = n - (room ? newWidth : 0);

    // Titles, then shrunk titles.
    auto titlesWidth = [&](int cap) {
        int w = 0;
        for (int i = 0; i < count; ++i) {
            w += widthOf(cut(titles[std::size_t(i)], cap)) + 4;
        }
        return count > 0 ? w + (count - 1) : 0;
    };
    int chosenCap = 0;
    for (int cap = kMaxTitle; cap >= kMinTitle; --cap) {
        if (titlesWidth(cap) <= avail) {
            chosenCap = cap;
            break;
        }
    }
    if (count == 0 || chosenCap > 0) {
        strip.mode = StripMode::Titles;
        strip.firstVisible = 0;
        strip.visibleCount = count;
        for (int i = 0; i < count; ++i) {
            if (i > 0) {
                drawGap();
            }
            drawTab(i, cut(titles[std::size_t(i)], chosenCap), true);
        }
        if (room) {
            drawNew();
        }
        strip.cells = row.take();
        return strip;
    }

    // Numbers: the active tab keeps its close button.
    auto numbersWidth = [&]() {
        int w = 0;
        for (int i = 0; i < count; ++i) {
            w += digits(i + 1) + 3 + (tabs[std::size_t(i)].active ? 1 : 0);
        }
        return w + (count - 1);
    };
    if (numbersWidth() <= avail) {
        strip.mode = StripMode::Numbers;
        strip.visibleCount = count;
        for (int i = 0; i < count; ++i) {
            if (i > 0) {
                drawGap();
            }
            drawTab(i, number(i), tabs[std::size_t(i)].active);
        }
        if (room) {
            drawNew();
        }
        strip.cells = row.take();
        return strip;
    }

    // A window of numbers with scroll buttons on both sides.
    strip.mode = StripMode::Scrolled;
    int active = 0;
    for (int i = 0; i < count; ++i) {
        if (tabs[std::size_t(i)].active) {
            active = i;
        }
    }
    auto tabWidth = [&](int i) {
        return digits(i + 1) + 3 + (tabs[std::size_t(i)].active ? 1 : 0);
    };
    const int buttons = 2 * 2; // " <" and "> " (two columns each).
    const int space = std::max(0, avail - buttons);
    auto shown = [&](int first) { // How many fit from `first`.
        int used = 0, k = 0;
        for (int i = first; i < count; ++i) {
            const int w = tabWidth(i) + (k > 0 ? 1 : 0);
            if (used + w > space) {
                break;
            }
            used += w;
            ++k;
        }
        return k;
    };
    int first = std::clamp(firstVisible, 0, std::max(0, count - 1));
    if (active < first) {
        first = active;
    }
    while (first < active && first + shown(first) <= active) {
        ++first;
    }
    // Do not leave blank space at the end while earlier tabs are hidden.
    while (first > 0 && first - 1 + shown(first - 1) >= count) {
        --first;
    }
    const int visible = shown(first);
    strip.firstVisible = first;
    strip.visibleCount = visible;
    const int leftBegin = row.column();
    row.put(U' ', col.dim, col.stripBg);
    row.put(U'<', first > 0 ? col.tabFg : col.dim, col.stripBg);
    region(leftBegin, row.column(), {StripHit::Kind::ScrollLeft, -1});
    for (int k = 0; k < visible; ++k) {
        if (k > 0) {
            drawGap();
        }
        drawTab(first + k,
                number(first + k),
                tabs[std::size_t(first) + std::size_t(k)].active);
    }
    // The buttons stay at fixed places: the right one at the right end, after
    // the new-tab button.
    if (room) {
        drawNew();
    }
    const int rightBegin = std::max(row.column(), n - 2);
    while (row.column() < rightBegin) {
        row.put(U' ', col.tabFg, col.stripBg);
    }
    const int rightAt = row.column();
    row.put(U'>', first + visible < count ? col.tabFg : col.dim, col.stripBg);
    row.put(U' ', col.dim, col.stripBg);
    region(rightAt, row.column(), {StripHit::Kind::ScrollRight, -1});
    strip.cells = row.take();
    return strip;
}

} // namespace frontend
