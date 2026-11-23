#pragma once

// The list of running terminals that `matterm --attach-tty` starts with: which
// row is chosen, what each row says, and how the list looks on a character
// screen. Pure: the daemon is asked by the caller, the screen is drawn by
// whoever has the host terminal.
#include "control.h"
#include "ttyinput.h"
#include "ttyscreen.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace frontend::ttymode {

struct ListEntry {
    std::string id;
    std::string title;  // The terminal's name, else a short form of its id.
    std::string what;   // What runs there: the command, else "shell".
    std::string folder; // The folder it is in (full), empty when unknown.
    bool inUse = false; // Another client has it: opening it takes it over.
    bool thisTerminal =
        false; // The terminal this program itself runs in: not to be attached.
};

// The name of the top folder of a path: "/home/a/projects/matterm" ->
// "matterm"; "/" for the root and for an empty path. A trailing slash does not
// count.
inline std::string folderName(const std::string &path) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const auto slash = trimmed.rfind('/');
    const std::string last =
        slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    if (last.empty()) {
        return "/";
    }
    std::string out;
    for (const char c : last) {
        out += (static_cast<unsigned char>(c) < 32 || c == 127) ? ' ' : c;
    }
    return out;
}

// A row of the list from the daemon's answers. `info` may be missing (the
// daemon did not answer).
inline ListEntry makeListEntry(const mtd::SessionEntry &entry,
                               const mtd::SessionInfo *info,
                               const std::string &ownSession = {}) {
    ListEntry out;
    out.id = entry.id;
    out.thisTerminal = !ownSession.empty() && entry.id == ownSession;
    out.inUse = entry.attached;
    const std::string &name = !entry.name.empty() ? entry.name
                              : info              ? info->name
                                                  : std::string();
    // The leftmost column: an explicit name wins, else the name of the top
    // folder (what the terminal is about), else - when the daemon did not say -
    // a short form of the id.
    out.title = !name.empty() ? name
                : info        ? folderName(info->cwd)
                              : "terminal " + entry.id.substr(0, 8);
    if (info) {
        if (info->busy && !info->command.empty()) {
            for (const std::string &part : info->command) {
                if (!out.what.empty()) {
                    out.what += ' ';
                }
                out.what += part;
            }
        }
        else {
            out.what = "shell";
        }
        out.folder = info->cwd;
        for (std::string *text : {&out.what, &out.folder}) {
            for (char &c : *text) {
                if (static_cast<unsigned char>(c) < 32 || c == 127) {
                    c = ' '; // A command line or a folder may hold anything;
                             // the screen must not see it.
                }
            }
        }
    }
    return out;
}

enum class ListKey {
    None,
    Up,
    Down,
    First,
    Last,
    PageUp,
    PageDown,
    Open,
    Refresh,
    Quit,
    Digit,
    New
};

struct ListAction {
    enum class Kind { None, Open, New, Refresh, Quit };
    Kind kind = Kind::None;
    std::string id; // Open.
};

// The rows are "New terminal" first, then the terminals of the daemon.
class ListModel {
public:
    // Keeps the chosen row when the same terminal is still there; else the
    // first terminal, or "New terminal" when there is none.
    void setEntries(std::vector<ListEntry> entries) {
        const std::string chosen = selected() ? selected()->id : std::string();
        entries_ = std::move(entries);
        index_ = entries_.empty() || preferNew_ ? 0 : 1;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!chosen.empty() && entries_[i].id == chosen) {
                index_ = i + 1;
            }
        }
    }
    // "New terminal" is the row chosen at first (matmux without arguments)
    // instead of the first terminal.
    void setPreferNew(bool on) {
        preferNew_ = on;
    }
    const std::vector<ListEntry> &entries() const {
        return entries_;
    }
    std::size_t rowCount() const {
        return entries_.size() + 1;
    }
    // 0 is "New terminal"; 1.. are the entries.
    std::size_t index() const {
        return index_;
    }
    bool newSelected() const {
        return index_ == 0;
    }
    const ListEntry *selected() const {
        return index_ >= 1 && index_ <= entries_.size() ? &entries_[index_ - 1]
                                                        : nullptr;
    }
    // A line under the list: what went wrong, what happened to the terminal
    // that was shown.
    void setMessage(std::string text) {
        message_ = std::move(text);
    }
    const std::string &message() const {
        return message_;
    }

    ListAction key(ListKey key, int digit = 0, int pageSize = 10) {
        ListAction action;
        const std::size_t last = entries_.size();
        switch (key) {
        case ListKey::Up:
            index_ = index_ > 0 ? index_ - 1 : 0;
            break;
        case ListKey::Down:
            index_ = std::min(index_ + 1, last);
            break;
        case ListKey::First:
            index_ = 0;
            break;
        case ListKey::Last:
            index_ = last;
            break;
        case ListKey::PageUp:
            index_ = index_ > std::size_t(pageSize)
                         ? index_ - std::size_t(pageSize)
                         : 0;
            break;
        case ListKey::PageDown:
            index_ =
                std::min(index_ + std::size_t(std::max(1, pageSize)), last);
            break;
        case ListKey::Digit: // The number shown in front of a terminal.
            if (digit >= 1 && std::size_t(digit) <= entries_.size()) {
                index_ = std::size_t(digit);
            }
            break;
        case ListKey::New:
            action.kind = ListAction::Kind::New;
            break;
        case ListKey::Open:
            if (index_ == 0) {
                action.kind = ListAction::Kind::New;
            }
            else if (const ListEntry *entry = selected()) {
                if (entry->thisTerminal) {
                    message_ = "You are in this terminal: attaching it would "
                               "feed it to itself";
                }
                else {
                    action.kind = ListAction::Kind::Open;
                    action.id = entry->id;
                }
            }
            break;
        case ListKey::Refresh:
            action.kind = ListAction::Kind::Refresh;
            break;
        case ListKey::Quit:
            action.kind = ListAction::Kind::Quit;
            break;
        case ListKey::None:
            break;
        }
        return action;
    }

private:
    std::vector<ListEntry> entries_;
    std::size_t index_ = 0;
    bool preferNew_ = false;
    std::string message_;
};

// What a decoded input event means in the list. A digit chooses that row (1-9).
inline ListKey listKeyOf(const InputEvent &event, int &digit) {
    using K = InputEncoder::Key;
    digit = 0;
    switch (event.kind) {
    case InputEvent::Kind::Key:
        switch (event.key) {
        case K::Up:
            return ListKey::Up;
        case K::Down:
            return ListKey::Down;
        case K::Home:
            return ListKey::First;
        case K::End:
            return ListKey::Last;
        case K::PageUp:
            return ListKey::PageUp;
        case K::PageDown:
            return ListKey::PageDown;
        case K::Enter:
            return ListKey::Open;
        case K::Escape:
            return ListKey::Quit;
        default:
            return ListKey::None;
        }
    case InputEvent::Kind::Bytes:
        // One keystroke: the caller feeds the bytes of a longer run one by one
        // ("jj" typed fast).
        if (event.text.size() == 1) {
            const char c = event.text[0];
            if (c == 'q' || c == 'Q' || c == 0x03) {
                return ListKey::Quit;
            }
            if (c == 'j') {
                return ListKey::Down;
            }
            if (c == 'k') {
                return ListKey::Up;
            }
            if (c == 'g') {
                return ListKey::First;
            }
            if (c == 'G') {
                return ListKey::Last;
            }
            if (c == 'n' || c == 'N') {
                return ListKey::New;
            }
            if (c == 'r' || c == 'R') {
                return ListKey::Refresh;
            }
            if (c >= '1' && c <= '9') {
                digit = c - '0';
                return ListKey::Digit;
            }
        }
        return ListKey::None;
    case InputEvent::Kind::Prefix:
        return ListKey::None;
    case InputEvent::Kind::Leave:
        return ListKey::Quit;
    case InputEvent::Kind::Mouse:
    case InputEvent::Kind::Paste:
        break;
    }
    return ListKey::None;
}

// Colours of the list: a quiet header and footer, the chosen row inverted.
inline constexpr ::tty::Rgb kBarFg{20, 20, 20};
inline constexpr ::tty::Rgb kBarBg{190, 190, 190};
inline constexpr ::tty::Rgb kDim{150, 150, 150};
inline constexpr ::tty::Rgb kWarn{240, 190, 90};

// The number of rows of the list that fit on a screen of `rows` rows.
inline int listCapacity(int rows) {
    return std::max(0, rows - 4); // Header, blank, (rows), message, footer.
}

// Draws the list over the whole canvas.
inline void renderList(const ListModel &model, ::tty::Canvas &canvas) {
    const int columns = canvas.columns();
    const int rows = canvas.rows();
    canvas.clear(kScreenBg);
    if (columns <= 0 || rows <= 0) {
        return;
    }
    fillRow(canvas, 0, kBarFg, kBarBg);
    put(canvas, 1, 0, "matterm: running terminals", kBarFg, kBarBg, true);
    const auto &entries = model.entries();
    const int capacity = listCapacity(rows);
    if (entries.empty()) {
        put(canvas, 1, 3, "No terminals are running in the daemon.", kDim);
    }
    // The visible part follows the chosen row.
    std::size_t first = 0;
    if (capacity > 0 && model.index() >= std::size_t(capacity)) {
        first = model.index() + 1 - std::size_t(capacity);
    }
    for (int line = 0;
         line < capacity && first + std::size_t(line) < model.rowCount();
         ++line) {
        const std::size_t i = first + std::size_t(line);
        const bool chosen = i == model.index();
        const ::tty::Rgb fg = chosen ? kBarFg : kScreenFg;
        const ::tty::Rgb bg = chosen ? kBarBg : kScreenBg;
        const int y = 2 + line;
        fillRow(canvas, y, fg, bg);
        if (i == 0) {
            int x = put(canvas, 1, y, "n ", chosen ? fg : kDim, bg);
            put(canvas, x, y, "New terminal", fg, bg, true);
            continue;
        }
        const ListEntry &entry = entries[i - 1];
        const std::string number = i < 10 ? std::to_string(i) : " ";
        int x = put(canvas, 1, y, number + " ", chosen ? fg : kDim, bg);
        x = put(canvas, x, y, entry.title, fg, bg, true);
        if (entry.thisTerminal) { // Before the long part, so that a narrow
                                  // screen still shows it.
            x = put(canvas, x, y, " (this terminal)", chosen ? fg : kWarn, bg);
        }
        else if (entry.inUse) {
            x = put(canvas, x, y, " (in use)", chosen ? fg : kWarn, bg);
        }
        x = put(canvas, x, y, "  " + entry.what, chosen ? fg : kScreenFg, bg);
        if (!entry.folder.empty()) {
            put(canvas, x, y, "  " + entry.folder, chosen ? fg : kDim, bg);
        }
    }
    const ListEntry *chosenEntry = model.selected();
    if (model.message().empty() && chosenEntry && chosenEntry->inUse &&
        !chosenEntry->thisTerminal) {
        put(canvas, 1, rows - 2, "In use: opening takes it over.", kWarn);
    }
    if (!model.message().empty()) {
        put(canvas, 1, rows - 2, model.message(), kWarn);
    }
    fillRow(canvas, rows - 1, kBarFg, kBarBg);
    put(canvas,
        1,
        rows - 1,
        "Up/Down choose   Enter open   n new   r refresh   q quit",
        kBarFg,
        kBarBg);
}

} // namespace frontend::ttymode
