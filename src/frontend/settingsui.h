#pragma once

// Settings-specific controller and layout, not a second widget framework.
// Rendering/input are the shared tty Canvas/Key types; file policy stays in
// SettingsWatcher. A commit rebases ONE edited value onto a freshly read file.
#include "commandregistry.h"
#include "settingswatch.h"
#include "themes.h"
#include "tty/canvas.h"
#include "tty/keys.h"
#include "tty/utf8.h"
#include "tty/width.h"

namespace frontend::settings {
class SettingsUI {
public:
    struct Row {
        std::string key, label, value, description, computer;
        int section = 0;
        bool addComputer = false;
    };
    explicit SettingsUI(SettingsWatcher &watcher)
        : watcher_(watcher) {
        if (!watcher.error().empty())
            message_ = watcher.error();
        else if (!watcher.warnings().empty())
            message_ = watcher.warnings().front();
    }
    bool running() const {
        return running_;
    }
    bool editing() const {
        return editing_;
    }
    const std::string &message() const {
        return message_;
    }
    void setQuery(std::string query) {
        query_ = std::move(query);
        selected_ = 0;
        sectionsFocus_ = false;
    }
    std::vector<Row> rows() const {
        std::vector<Row> all;
        const auto &settings = watcher_.settings();
        for (const auto &d : definitions()) {
            if (d.type == Type::Computers)
                continue;
            const int section =
                std::string(d.key) == "scrollbackRows" ||
                        std::string(d.key) == "confirmClose" ||
                        std::string(d.key) == "terminalListPreview"
                    ? 1
                    : 0;
            all.push_back({d.key,
                           d.label,
                           value(settings, d.key, {}),
                           d.text,
                           {},
                           section,
                           false});
        }
        for (const auto &name : settings.computerNames())
            all.push_back(
                {"computers." + name,
                 name,
                 settings.computerChoice(name),
                 "Theme for this computer; auto chooses from its name.",
                 name,
                 2,
                 false});
        all.push_back(
            {"computers.add",
             "Add computer...",
             "+",
             "Enter the --daemon-host name, then choose a theme or auto.",
             {},
             2,
             true});
        std::vector<Row> result;
        if (query_.empty()) {
            for (const auto &r : all)
                if (r.section == section_)
                    result.push_back(r);
        }
        else {
            std::vector<CommandEntry> entries;
            for (const auto &r : all)
                entries.push_back(
                    {r.key, r.label, r.description, std::nullopt, {}, {}});
            for (const auto &ranked : searchCommands(entries, query_)) {
                const auto index = std::size_t(ranked.entry - entries.data());
                result.push_back(all[index]);
            }
        }
        return result;
    }
    bool poll() {
        const auto event = watcher_.poll();
        if (event == SettingsWatcher::Event::Failed)
            message_ = watcher_.error();
        if (event == SettingsWatcher::Event::Changed && !editing_)
            message_ = "Reloaded external settings";
        clampSelection();
        return event != SettingsWatcher::Event::None;
    }
    void handle(const tty::Key &key) {
        using K = tty::Key::Type;
        if (key.is(K::Ctrl) && (key.text == "q" || key.text == "c")) {
            running_ = false;
            return;
        }
        if (!interactive_ && !key.is(K::Escape))
            return;
        if (key.is(K::Mouse)) {
            mouseKey(key);
            return;
        }
        if (editing_) {
            editKey(key);
            return;
        }
        if (key.is(K::Escape)) {
            if (!query_.empty()) {
                setQuery({});
                return;
            }
            running_ = false;
            return;
        }
        if (key.is(K::Tab)) {
            sectionsFocus_ = !sectionsFocus_;
            return;
        }
        if (key.is(K::Text)) {
            setQuery(query_ + key.text);
            return;
        }
        if (key.is(K::Backspace)) {
            popUtf8(query_);
            selected_ = 0;
            return;
        }
        if (sectionsFocus_) {
            if (key.is(K::Up) || key.is(K::Down)) {
                section_ = (section_ + (key.is(K::Up) ? 2 : 1)) % 3;
                query_.clear();
                selected_ = 0;
            }
            if (key.is(K::Right) || key.is(K::Enter))
                sectionsFocus_ = false;
            return;
        }
        const auto items = rows();
        if (items.empty())
            return;
        clampSelection();
        if (key.is(K::Up) && selected_ > 0)
            --selected_;
        else if (key.is(K::Down) && selected_ + 1 < items.size())
            ++selected_;
        else if (key.is(K::Home))
            selected_ = 0;
        else if (key.is(K::End))
            selected_ = items.size() - 1;
        else if (key.is(K::Left))
            sectionsFocus_ = true;
        else if (key.is(K::Enter) || key.is(K::Right))
            begin(items[selected_]);
    }
    void draw(tty::Canvas &canvas) {
        interactive_ = canvas.columns() >= 42 && canvas.rows() >= 14;
        const auto &theme = themes::theme(
            editing_ && !hostStage_ && edit_.key == "theme" && !choices_.empty()
                ? choices_[choice_]
                : watcher_.settings().choice("theme"));
        const tty::Rgb bg{18, 19, 23}, fg{220, 223, 230}, dim{155, 163, 177};
        const tty::Rgb accent{theme.strip.r, theme.strip.g, theme.strip.b};
        canvas.clear(bg);
        if (!interactive_) {
            canvas.text(0, 0, "Settings: resize terminal", fg, bg);
            canvas.text(0, 1, "Ctrl+Q exits", dim, bg);
            return;
        }
        layout_ = layout(canvas.columns(), canvas.rows());
        const auto [width,
                    x,
                    top,
                    bottom,
                    leftWidth,
                    right,
                    rightWidth,
                    visible,
                    first] = layout_;
        const auto line = [&](int px,
                              int py,
                              const std::string &s,
                              int limit,
                              tty::Rgb color,
                              tty::Rgb background,
                              bool bold = false) {
            // Canvas::text deliberately advances one cell per code point for
            // existing matrix clients. Settings names need terminal widths.
            int column = px;
            for (const auto cp : tty::decodeUtf8(fit(s, limit))) {
                const int cells = tty::cellWidth(cp);
                if (cells == 0)
                    continue; // Canvas has no combining clusters.
                canvas.put(column, py, cp, color, background, bold);
                column += cells;
            }
        };
        line(x, top, "matterm  /  Settings", width, fg, bg, true);
        line(x,
             top + 2,
             "Search: " + query_ + (editing_ ? "" : "_"),
             width,
             dim,
             bg);
        for (int i = 0; i < width; ++i)
            canvas.put(x + i, top + 3, U'─', dim, bg);
        const char *sections[] = {"Appearance", "Terminal", "Computers"};
        for (int i = 0; i < 3; ++i) {
            const bool active = i == section_;
            const auto background = active && sectionsFocus_ ? accent : bg;
            line(x,
                 top + 5 + i * 2,
                 std::string(active ? "> " : "  ") + sections[i],
                 leftWidth,
                 active ? fg : dim,
                 background,
                 active);
        }
        for (int y = top + 4; y < bottom - 4; ++y)
            canvas.put(x + leftWidth, y, U'│', dim, bg);
        const auto items = rows();
        if (items.empty())
            line(right, top + 5, "No matching settings", rightWidth, dim, bg);
        for (std::size_t i = first; i < items.size() && i < first + visible;
             ++i) {
            const int y = top + 5 + int(i - first) * 2;
            const bool selected = i == selected_ && !sectionsFocus_;
            const auto background = selected ? accent : bg;
            for (int j = 0; j < rightWidth; ++j)
                canvas.put(right + j, y, U' ', fg, background);
            const std::string label =
                std::string(selected ? "> " : "  ") + items[i].label;
            const int valueWidth = std::min(18, rightWidth / 2);
            line(right,
                 y,
                 label,
                 rightWidth - valueWidth - 1,
                 fg,
                 background,
                 selected);
            line(right + rightWidth - valueWidth,
                 y,
                 "[ " + items[i].value + " ]",
                 valueWidth,
                 fg,
                 background);
        }
        if (!items.empty() && selected_ < items.size() && !editing_) {
            line(right,
                 bottom - 5,
                 items[selected_].description,
                 rightWidth,
                 dim,
                 bg);
            const auto *d = definition(items[selected_].key);
            if (d && d->needsRestart)
                line(right,
                     bottom - 4,
                     "Applies to new windows / terminals",
                     rightWidth,
                     dim,
                     bg);
        }
        if (editing_) {
            const std::string shownValue =
                hostStage_ || choices_.empty()
                    ? buffer_ + "_"
                    : "< " + choices_[choice_] + " >";
            const auto label = hostStage_ ? std::string("Name") : edit_.label;
            // Reserve the entire choice before clipping a potentially long
            // label (computer names may be up to 128 bytes).
            const auto labelWidth =
                std::max(0, rightWidth - int(shownValue.size()) - 2);
            const std::string prompt =
                fit(label, labelWidth) + ": " + shownValue;
            line(right, bottom - 5, prompt, rightWidth, fg, accent, true);
            line(right,
                 bottom - 4,
                 "[Apply]  [Cancel]  Enter / Esc",
                 rightWidth,
                 dim,
                 bg);
        }
        line(x, bottom - 2, message_, width, dim, bg);
        line(x,
             bottom,
             "Ctrl+Q quit  Esc back  Enter edit  Tab sections  Type search",
             width,
             dim,
             bg);
    }

private:
    struct Layout {
        int width = 0, x = 0, top = 0, bottom = 0, leftWidth = 0, right = 0,
            rightWidth = 0;
        std::size_t visible = 0, first = 0;
    };
    Layout layout(int columns, int height) const {
        Layout l;
        l.width = std::min(100, columns - 4);
        l.x = (columns - l.width) / 2;
        l.top = height > 32 ? (height - 30) / 2 : 1;
        l.bottom = std::min(height - 2, l.top + 29);
        l.leftWidth = l.width >= 70 ? 22 : 15;
        l.right = l.x + l.leftWidth + 2;
        l.rightWidth = l.width - l.leftWidth - 2;
        l.visible = std::size_t(std::max(1, (l.bottom - 5 - (l.top + 5)) / 2));
        l.first = selected_ >= l.visible ? selected_ - l.visible + 1 : 0;
        return l;
    }
    void mouseKey(const tty::Key &key) {
        if (!key.press || key.motion || key.button != 0)
            return;
        const auto &l = layout_;
        const int x = key.column, y = key.row;
        if (x < l.x || x >= l.x + l.width)
            return;
        if (editing_) {
            if (y != l.bottom - 4)
                return;
            tty::Key action;
            if (x >= l.right && x < l.right + 7)
                action.type = tty::Key::Type::Enter;
            else if (x >= l.right + 9 && x < l.right + 17)
                action.type = tty::Key::Type::Escape;
            else
                return;
            editKey(action);
            return;
        }
        if (y == l.top + 2) {
            sectionsFocus_ = false;
            return;
        }
        if (x < l.x + l.leftWidth) {
            for (int i = 0; i < 3; ++i) {
                if (y == l.top + 5 + i * 2) {
                    section_ = i;
                    query_.clear();
                    selected_ = 0;
                    sectionsFocus_ = true;
                    return;
                }
            }
        }
        if (x < l.right || x >= l.right + l.rightWidth || y < l.top + 5)
            return;
        const int offset = y - (l.top + 5);
        if (offset % 2 != 0 || std::size_t(offset / 2) >= l.visible)
            return;
        const auto items = rows();
        const auto index = l.first + std::size_t(offset / 2);
        if (index >= items.size())
            return;
        selected_ = index;
        sectionsFocus_ = false;
        begin(items[index]);
    }
    Layout layout_;
    static std::string value(const Settings &s,
                             const std::string &key,
                             const std::string &computer) {
        if (!computer.empty())
            return s.computerChoice(computer);
        const auto *d = definition(key);
        if (!d)
            return {};
        if (d->type == Type::Bool)
            return s.flag(key) ? "true" : "false";
        if (d->type == Type::Integer)
            return std::to_string(s.integer(key));
        return s.choice(key);
    }
    static void popUtf8(std::string &s) {
        if (s.empty())
            return;
        auto pos = s.size() - 1;
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xc0) == 0x80)
            --pos;
        s.resize(pos);
    }
    static std::string fit(const std::string &text, int width) {
        std::string out;
        int used = 0;
        for (auto cp : tty::decodeUtf8(text)) {
            if (cp < 32 || cp == 127)
                cp = U' '; // Names from a file never become terminal controls.
            const int n = tty::cellWidth(cp);
            if (used + n > width)
                break;
            tty::appendUtf8(out, cp);
            used += n;
        }
        return out;
    }
    void clampSelection() {
        const auto n = rows().size();
        if (selected_ >= n)
            selected_ = n ? n - 1 : 0;
    }
    void begin(Row row) {
        edit_ = std::move(row);
        editing_ = true;
        message_.clear();
        choices_.clear();
        choice_ = 0;
        hostStage_ = edit_.addComputer;
        replaceBuffer_ = true;
        buffer_ = hostStage_ ? "" : edit_.value;
        before_ = edit_.value;
        if (!hostStage_)
            fillChoices();
    }
    void fillChoices() {
        if (!edit_.computer.empty()) {
            choices_ = {"auto"};
            choices_.insert(
                choices_.end(), themeIds().begin(), themeIds().end());
        }
        else if (const auto *d = definition(edit_.key)) {
            if (d->type == Type::Choice)
                choices_ = d->choices;
            else if (d->type == Type::Bool)
                choices_ = {"true", "false"};
        }
        const auto it = std::find(choices_.begin(), choices_.end(), before_);
        choice_ = it == choices_.end() ? 0 : std::size_t(it - choices_.begin());
    }
    void editKey(const tty::Key &key) {
        using K = tty::Key::Type;
        if (key.is(K::Escape)) {
            editing_ = false;
            hostStage_ = false;
            message_ = "Edit cancelled; earlier changes remain saved";
            return;
        }
        if (!choices_.empty() && !hostStage_) {
            if (key.is(K::Up) || key.is(K::Left))
                choice_ = (choice_ + choices_.size() - 1) % choices_.size();
            else if (key.is(K::Down) || key.is(K::Right))
                choice_ = (choice_ + 1) % choices_.size();
            else if (key.is(K::Home))
                choice_ = 0;
            else if (key.is(K::End))
                choice_ = choices_.size() - 1;
        }
        else if (key.is(K::Text)) {
            if (replaceBuffer_) {
                buffer_.clear();
                replaceBuffer_ = false;
            }
            if (buffer_.size() + key.text.size() <= 128)
                buffer_ += key.text;
        }
        else if (key.is(K::Backspace) || key.is(K::Delete)) {
            if (replaceBuffer_) {
                buffer_.clear();
                replaceBuffer_ = false;
            }
            else
                popUtf8(buffer_);
        }
        if (!key.is(K::Enter))
            return;
        if (hostStage_) {
            if (!validComputerName(buffer_)) {
                message_ = "Enter a valid computer name (1-128 bytes)";
                return;
            }
            edit_.computer = buffer_;
            edit_.label = buffer_;
            edit_.key = "computers." + buffer_;
            before_ = watcher_.settings().computerChoice(buffer_);
            hostStage_ = false;
            fillChoices();
            return;
        }
        // Always read again, even if a timestamp and size happen to be equal.
        // Apply only this field to that document, preserving other users'
        // edits.
        if (watcher_.reloadNow() == SettingsWatcher::Event::Failed ||
            !watcher_.error().empty()) {
            message_ = "Cannot save: " + watcher_.error();
            return;
        }
        if (value(watcher_.settings(), edit_.key, edit_.computer) != before_) {
            message_ = "This value changed externally; Esc and reopen it";
            return;
        }
        auto changed = watcher_.settings();
        std::string error;
        const auto next = choices_.empty() ? buffer_ : choices_[choice_];
        const bool ok = edit_.computer.empty()
                            ? changed.set(edit_.key, next, error)
                            : changed.setComputer(edit_.computer, next, error);
        if (!ok || !watcher_.save(changed, error)) {
            message_ = "Cannot save: " + error;
            return;
        }
        editing_ = false;
        message_ = "Saved  /  windows follow within one second";
        clampSelection();
    }
    SettingsWatcher &watcher_;
    std::string query_, message_, buffer_, before_;
    std::vector<std::string> choices_;
    Row edit_;
    std::size_t selected_ = 0, choice_ = 0;
    int section_ = 0;
    bool sectionsFocus_ = false, editing_ = false, hostStage_ = false;
    bool replaceBuffer_ = false, running_ = true, interactive_ = true;
};
} // namespace frontend::settings
