#pragma once

// The keyboard shortcut help view and the which-key box (docs/TABS_PLAN.md section 8).
// Both are built from the command registry, so they cannot disagree with the keys.
#include "commandregistry.h"
#include "layer.h"
#include "listview.h"

#include <algorithm>
#include <string>
#include <vector>

namespace frontend::ui {

// One line of the help view: a name and the keys of each kind. Section headings have
// only `name`.
struct ShortcutRow {
    std::string name;
    std::string desktop;
    std::string prefix;
    bool heading = false;
};

inline std::string joinKeys(const std::vector<std::string> &keys) {
    std::string s;
    for (const auto &k : keys) {
        if (!s.empty()) s += ", ";
        s += k;
    }
    return s;
}

// Everything that has keys, in registry order, then the fixed keys of copy mode.
// `copyAndPaste` rows come from the desktop table (they are not registry commands).
inline std::vector<ShortcutRow> shortcutRows(const std::vector<CommandEntry> &entries,
                                             bool showDesktop,
                                             bool showPrefix) {
    std::vector<ShortcutRow> rows;
    auto section = [&rows](const char *title) { rows.push_back({title, {}, {}, true}); };
    section("Commands");
    for (const auto &e : entries) {
        ShortcutRow r{e.name, showDesktop ? joinKeys(e.desktopKeys) : std::string(),
                      showPrefix ? joinKeys(e.prefixKeys) : std::string(), false};
        if (r.desktop.empty() && r.prefix.empty()) continue;
        rows.push_back(std::move(r));
    }
    if (showDesktop) {
        for (const auto &d : desktopShortcuts()) {
            if (d.action == DesktopAction::Copy) rows.push_back({"Copy selection", formatChord(d.chord), {}, false});
            if (d.action == DesktopAction::Paste) rows.push_back({"Paste", formatChord(d.chord), {}, false});
        }
    }
    section("Copy / scroll mode");
    const char *const copy[][2] = {
        {"Move the cursor", "h j k l, arrows"},
        {"Word / line start / line end", "w b e, 0 ^ $"},
        {"Page up / down, top / bottom", "PgUp PgDn, g G"},
        {"Select characters / lines / block", "v or Space, V, Ctrl+V"},
        {"Search forward / backward", "/  ?   (next n, previous N)"},
        {"Copy and leave", "y or Enter"},
        {"Leave", "Esc or q"},
    };
    for (const auto &c : copy) rows.push_back({c[0], {}, c[1], false});
    return rows;
}

// A scrollable list of all shortcuts. Esc, q or Enter close it.
class ShortcutsView final : public Layer {
public:
    ShortcutsView(const std::vector<CommandEntry> &entries, bool showDesktop, bool showPrefix)
        : rows_(shortcutRows(entries, showDesktop, showPrefix))
        , showDesktop_(showDesktop)
        , showPrefix_(showPrefix) {
        list_.setCount(rows_.size());
    }

    std::size_t rowCount() const { return rows_.size(); }
    std::size_t firstVisible() const { return list_.first(); }

    CellSize wanted(int windowColumns, int windowRows) const override {
        CellSize s;
        s.columns = std::min(windowColumns, std::clamp(windowColumns - 2, 12, 78));
        s.rows = std::min(windowRows, std::clamp(windowRows - 3, 5, int(rows_.size()) + 4));
        return s;
    }
    void resize(int columns, int rows) override {
        columns_ = std::max(columns, 0);
        rows_n_ = std::max(rows, 0);
        list_.setVisible(std::size_t(std::max(rows_n_ - 3, 1)));
    }

    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 4 || rows_n_ < 4) return;
        cells.clear(FChar{U' ', kFg, kBorder});
        for (int y = 1; y < rows_n_ - 1; ++y) {
            for (int x = 1; x < columns_ - 1; ++x) cells.at(std::size_t(x), std::size_t(y)) = FChar{U' ', kFg, kBackground};
        }
        const int width = columns_ - 2;
        put(cells, 2, 1, "Keyboard shortcuts", kAccent, width - 1);
        const std::string hint = "Esc closes";
        if (width > int(hint.size()) + 24) put(cells, columns_ - 1 - int(hint.size()) - 1, 1, hint, kDim, width);
        // Columns: name, then the desktop and prefix keys.
        int nameWidth = std::clamp(width / 3, 12, 34);
        int deskWidth = showDesktop_ ? std::clamp(width / 4, 12, 22) : 0;
        const int listRows = rows_n_ - 3;
        for (int line = 0; line < listRows; ++line) {
            const std::size_t index = list_.first() + std::size_t(line);
            if (index >= rows_.size()) break;
            const auto &r = rows_[index];
            const int y = 2 + line;
            if (r.heading) {
                put(cells, 2, y, r.name, kAccent, width - 1, CellAttribute::Bold);
                continue;
            }
            put(cells, 2, y, r.name, kFg, nameWidth - 1);
            int x = 2 + nameWidth;
            if (showDesktop_) {
                put(cells, x, y, r.desktop, kKeys, deskWidth - 1);
                x += deskWidth;
            }
            if (showPrefix_) put(cells, x, y, r.prefix, kKeys, columns_ - 1 - x);
        }
        if (rows_.size() > list_.visible() && rows_n_ > 4) {
            // A scroll hint at the border: where the window of lines is.
            const int track = rows_n_ - 4;
            const int thumb = std::clamp(int(std::size_t(track) * list_.first() / rows_.size()), 0, track - 1);
            cells.at(std::size_t(columns_ - 1), std::size_t(2 + thumb)) = FChar{U' ', kFg, kAccent};
        }
    }

    LayerOutcome key(const KeyChord &chord) override {
        LayerOutcome out;
        if (chord.ctrl) {
            if (foldKey(chord.key) == 'n') list_.scroll(1);
            if (foldKey(chord.key) == 'p') list_.scroll(-1);
            return out;
        }
        switch (chord.key) {
        case keys::Escape:
        case keys::Enter:
        case 'q':
            out.close = true;
            break;
        case keys::Up:
        case 'k':
            list_.scroll(-1);
            break;
        case keys::Down:
        case 'j':
            list_.scroll(1);
            break;
        case keys::PageUp:
            list_.scroll(-int(list_.visible()));
            break;
        case keys::PageDown:
        case ' ':
            list_.scroll(int(list_.visible()));
            break;
        case keys::Home:
        case 'g':
            list_.scroll(-int(rows_.size()));
            break;
        case keys::End:
        case 'G':
            list_.scroll(int(rows_.size()));
            break;
        default:
            break;
        }
        return out;
    }
    LayerOutcome click(int, int) override { return {}; }
    void wheel(double amount) override {
        if (amount != 0) list_.scroll(amount > 0 ? -3 : 3);
    }

private:
    static constexpr Rgb8 kBorder{62, 84, 128};
    static constexpr Rgb8 kBackground{28, 32, 40};
    static constexpr Rgb8 kFg{230, 230, 230};
    static constexpr Rgb8 kDim{170, 178, 190};
    static constexpr Rgb8 kKeys{190, 200, 215};
    static constexpr Rgb8 kAccent{120, 190, 255};

    static void put(CellBuffer &cells, int x, int y, const std::string &text, Rgb8 fg, int maxColumns,
                    CellAttribute attributes = CellAttribute::None) {
        for (int i = 0; i < int(text.size()) && i < maxColumns; ++i) {
            FChar c{char32_t(static_cast<unsigned char>(text[std::size_t(i)])), fg, kBackground};
            c.attributes = attributes;
            if (x + i >= 0 && x + i < int(cells.columns())) cells.at(std::size_t(x + i), std::size_t(y)) = c;
        }
    }

    std::vector<ShortcutRow> rows_;
    bool showDesktop_, showPrefix_;
    ListView list_;
    int columns_ = 0;
    int rows_n_ = 0;
};

// The lines of the which-key box: "c   New tab". Keys of one command share a line, the
// "Go to tab N" digits one more.
inline std::vector<std::string> whichKeyLines(const PrefixKeys::Config &prefix) {
    std::vector<std::string> lines;
    const auto entries = buildCommandRegistry(prefix);
    bool tabDigits = false;
    for (const auto &e : entries) {
        if (e.prefixKeys.empty()) continue;
        if (e.call && e.call->command == Command::SelectTab) {
            if (tabDigits) continue;
            tabDigits = true;
            lines.push_back("1-0  Go to tab");
            continue;
        }
        std::string keys;
        for (const auto &k : e.prefixKeys) {
            const auto space = k.find(' ');
            std::string second = space == std::string::npos ? k : k.substr(space + 1);
            if (!keys.empty()) keys += "/";
            keys += second;
        }
        lines.push_back(keys + "  " + e.name);
    }
    return lines;
}

// The box that appears while the prefix key waits: the keys that can follow. No input.
class WhichKey final : public Layer {
public:
    explicit WhichKey(const PrefixKeys::Config &prefix)
        : lines_(whichKeyLines(prefix))
        , title_(formatChord(prefix.prefix) + " then") {}

    CellSize wanted(int windowColumns, int windowRows) const override {
        const int perColumn = std::max(windowRows - 4, 3);
        const int columns = std::max(1, (int(lines_.size()) + perColumn - 1) / perColumn);
        CellSize s;
        s.columns = std::min(windowColumns, 2 + columns * (maxWidth() + 2));
        s.rows = std::min(windowRows, 3 + std::min(int(lines_.size()), perColumn));
        return s;
    }
    void resize(int columns, int rows) override {
        columns_ = columns;
        rows_ = rows;
    }
    void render(CellBuffer &cells, CursorState &cursor) const override {
        cursor.visible = false;
        if (columns_ < 4 || rows_ < 3) return;
        cells.clear(FChar{U' ', kFg, kBorder});
        for (int y = 1; y < rows_ - 1; ++y) {
            for (int x = 1; x < columns_ - 1; ++x) cells.at(std::size_t(x), std::size_t(y)) = FChar{U' ', kFg, kBackground};
        }
        put(cells, 2, 1, title_, kAccent);
        const int perColumn = std::max(rows_ - 3, 1);
        const int columnWidth = maxWidth() + 2;
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            const int column = int(i) / perColumn;
            const int x = 2 + column * columnWidth;
            if (x >= columns_ - 1) break;
            put(cells, x, 2 + int(i) % perColumn, lines_[i], kFg);
        }
    }
    LayerOutcome key(const KeyChord &) override { return {}; }
    LayerOutcome click(int, int) override { return {}; }
    void wheel(double) override {}

private:
    static constexpr Rgb8 kBorder{62, 84, 128};
    static constexpr Rgb8 kBackground{28, 32, 40};
    static constexpr Rgb8 kFg{230, 230, 230};
    static constexpr Rgb8 kAccent{120, 190, 255};

    int maxWidth() const {
        std::size_t w = title_.size();
        for (const auto &l : lines_) w = std::max(w, l.size());
        return int(w);
    }
    void put(CellBuffer &cells, int x, int y, const std::string &text, Rgb8 fg) const {
        for (int i = 0; i < int(text.size()); ++i) {
            if (x + i >= columns_ - 1 || y >= rows_ - 1) return;
            cells.at(std::size_t(x + i), std::size_t(y)) =
                FChar{char32_t(static_cast<unsigned char>(text[std::size_t(i)])), fg, kBackground};
        }
    }

    std::vector<std::string> lines_;
    std::string title_;
    int columns_ = 0;
    int rows_ = 0;
};

} // namespace frontend::ui
