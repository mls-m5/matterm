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

namespace frontend::tty {

struct ListEntry {
    std::string id;
    std::string title; // The terminal's name, else a short form of its id.
    std::string what; // What runs there: the command, else "shell in <folder>".
    bool inUse = false; // Another client has it: opening it takes it over.
};

// A row of the list from the daemon's answers. `info` may be missing (the
// daemon did not answer).
inline ListEntry makeListEntry(const mtd::SessionEntry &entry,
                               const mtd::SessionInfo *info) {
    ListEntry out;
    out.id = entry.id;
    out.inUse = entry.attached;
    const std::string &name = !entry.name.empty() ? entry.name
                              : info              ? info->name
                                                  : std::string();
    out.title = !name.empty() ? name : "terminal " + entry.id.substr(0, 8);
    if (info) {
        if (info->busy && !info->command.empty()) {
            for (const std::string &part : info->command) {
                if (!out.what.empty()) {
                    out.what += ' ';
                }
                out.what += part;
            }
        }
        else if (!info->cwd.empty()) {
            out.what = "shell in " + info->cwd;
        }
        else {
            out.what = "shell";
        }
        for (char &c : out.what) {
            if (static_cast<unsigned char>(c) < 32 || c == 127) {
                c = ' '; // A command line may hold anything; the screen must
                         // not see it.
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
    Digit
};

struct ListAction {
    enum class Kind { None, Open, Refresh, Quit };
    Kind kind = Kind::None;
    std::string id; // Open.
};

class ListModel {
public:
    // Keeps the chosen row when the same terminal is still there.
    void setEntries(std::vector<ListEntry> entries) {
        std::string chosen = selected() ? selected()->id : std::string();
        entries_ = std::move(entries);
        index_ = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!chosen.empty() && entries_[i].id == chosen) {
                index_ = i;
            }
        }
    }
    const std::vector<ListEntry> &entries() const {
        return entries_;
    }
    std::size_t index() const {
        return index_;
    }
    const ListEntry *selected() const {
        return index_ < entries_.size() ? &entries_[index_] : nullptr;
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
        const std::size_t last = entries_.empty() ? 0 : entries_.size() - 1;
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
        case ListKey::Digit:
            if (digit >= 1 && std::size_t(digit) <= entries_.size()) {
                index_ = std::size_t(digit) - 1;
            }
            break;
        case ListKey::Open:
            if (const ListEntry *entry = selected()) {
                action.kind = ListAction::Kind::Open;
                action.id = entry->id;
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
            if (c == 'r' || c == 'R') {
                return ListKey::Refresh;
            }
            if (c >= '1' && c <= '9') {
                digit = c - '0';
                return ListKey::Digit;
            }
        }
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
inline constexpr Rgb8 kBarFg{20, 20, 20};
inline constexpr Rgb8 kBarBg{190, 190, 190};
inline constexpr Rgb8 kDim{150, 150, 150};
inline constexpr Rgb8 kWarn{240, 190, 90};

// The number of rows of the list that fit on a screen of `rows` rows.
inline int listCapacity(int rows) {
    return std::max(0, rows - 4); // Header, blank, (rows), message, footer.
}

inline ScreenImage renderList(const ListModel &model, int columns, int rows) {
    ScreenImage image(columns, rows);
    if (columns <= 0 || rows <= 0) {
        return image;
    }
    fillRow(image, 0, kBarFg, kBarBg);
    put(image,
        1,
        0,
        "matterm: running terminals",
        kBarFg,
        kBarBg,
        CellAttribute::Bold);
    const auto &entries = model.entries();
    const int capacity = listCapacity(rows);
    if (entries.empty()) {
        put(image, 1, 2, "No terminals are running in the daemon.", kDim);
    }
    // The visible part follows the chosen row.
    std::size_t first = 0;
    if (capacity > 0 && model.index() >= std::size_t(capacity)) {
        first = model.index() + 1 - std::size_t(capacity);
    }
    for (int line = 0;
         line < capacity && first + std::size_t(line) < entries.size();
         ++line) {
        const std::size_t i = first + std::size_t(line);
        const ListEntry &entry = entries[i];
        const bool chosen = i == model.index();
        const Rgb8 fg = chosen ? kBarFg : kScreenFg;
        const Rgb8 bg = chosen ? kBarBg : kScreenBg;
        const int y = 2 + line;
        fillRow(image, y, fg, bg);
        std::string number = i < 9 ? std::to_string(i + 1) : " ";
        int x = put(image, 1, y, number + " ", chosen ? fg : kDim, bg);
        x = put(image, x, y, entry.title, fg, bg, CellAttribute::Bold);
        if (entry.inUse) { // Before the long part, so that a narrow screen
                           // still shows it.
            x = put(image, x, y, " (in use)", chosen ? fg : kWarn, bg);
        }
        put(image, x, y, "  " + entry.what, chosen ? fg : kDim, bg);
    }
    const ListEntry *chosenEntry = model.selected();
    if (model.message().empty() && chosenEntry && chosenEntry->inUse) {
        put(image,
            1,
            rows - 2,
            "In use: opening takes it over.",
            kWarn);
    }
    if (!model.message().empty()) {
        put(image, 1, rows - 2, model.message(), kWarn);
    }
    fillRow(image, rows - 1, kBarFg, kBarBg);
    put(image,
        1,
        rows - 1,
        "Up/Down choose   Enter open   r refresh   q quit",
        kBarFg,
        kBarBg);
    return image;
}

} // namespace frontend::tty
