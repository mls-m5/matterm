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
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frontend::ttymode {

struct ListEntry {
    std::string id;
    std::string title;  // The terminal's name, else a short form of its id.
    std::string what;   // What runs there: the command, else "shell".
    std::string folder; // The folder it is in (full), empty when unknown.
    std::optional<bool>
        foregroundBusy; // false means shell foreground, NOT idle prompt.
    std::optional<std::uint64_t>
        outputAgeMs; // Snapshot age; not a local wall-clock timestamp.
    std::optional<std::string>
        preview;        // Recent output, not a last-command/screen-row claim.
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
        // The legacy busy bit conflates unknown /proc state and shell
        // foreground. Only explicit new metadata may classify foregroundBusy.
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
    New,
    End, // x or Delete: end the chosen terminal (after a question).
    Yes  // y: the answer "end it" while the question is open.
};

struct ListAction {
    enum class Kind { None, Open, New, Refresh, Quit, End };
    Kind kind = Kind::None;
    std::string id;    // Open, End.
    std::string title; // End: its name, for the note afterwards.
};

// One line: End "matterm" (vim)? y = end, n/Esc = keep. Names what runs when
// known, and says when another client has it open.
inline std::string endQuestion(const ListEntry &entry) {
    std::string text = "End \"" + entry.title + "\"";
    if (!entry.what.empty()) {
        text += " (" + entry.what + ")";
    }
    text += "?";
    if (entry.inUse) {
        text += " (also open in another client)";
    }
    return text + " y = end, n/Esc = keep";
}

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
        confirming_.clear(); // The list changed under the question.
        if (anchor_) {       // A terminal was ended: stay near where it was.
            index_ =
                entries_.empty()
                    ? 0
                    : std::clamp<std::size_t>(*anchor_, 1, entries_.size());
            anchor_.reset();
        }
    }
    // "New terminal" is the row chosen at first (matmux without arguments)
    // instead of the first terminal.
    void setPreferNew(bool on) {
        preferNew_ = on;
    }
    void setPreviewEnabled(bool enabled) {
        previewEnabled_ = enabled;
    }
    bool previewEnabled() const {
        return previewEnabled_;
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
    // The question while ending a terminal is asked, else empty.
    const std::string &question() const {
        return question_;
    }
    // Ending failed: the list did not change, so the row needs no nudge.
    void cancelAnchor() {
        anchor_.reset();
    }
    bool confirming() const {
        return !confirming_.empty();
    }

    ListAction key(ListKey key, int digit = 0, int pageSize = 10) {
        ListAction action;
        if (confirming()) { // The answer is y; anything else keeps the terminal
                            // (the default).
            const ListEntry *entry = selected();
            const bool yes =
                key == ListKey::Yes && entry && entry->id == confirming_;
            if (yes) {
                action.kind = ListAction::Kind::End;
                action.id = entry->id;
                action.title = entry->title;
                anchor_ = index_;
            }
            confirming_.clear();
            question_.clear();
            return action;
        }
        if (key == ListKey::End) {
            if (const ListEntry *entry = selected()) {
                if (entry->thisTerminal) {
                    message_ = "You are in this terminal: ending it would end "
                               "this program too";
                }
                else {
                    confirming_ = entry->id;
                    question_ = endQuestion(*entry);
                    message_.clear();
                }
            }
            return action;
        }
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
        case ListKey::End: // Handled above.
        case ListKey::Yes:
        case ListKey::None:
            break;
        }
        return action;
    }

private:
    std::vector<ListEntry> entries_;
    std::size_t index_ = 0;
    bool preferNew_ = false;
    bool previewEnabled_ = true;
    std::string message_;
    std::string question_;
    std::string confirming_;            // The id the question is about.
    std::optional<std::size_t> anchor_; // Where a terminal was ended.
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
        case K::Delete:
            return ListKey::End;
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
            if (c == 'x' || c == 'X') {
                return ListKey::End;
            }
            if (c == 'y' || c == 'Y') {
                return ListKey::Yes;
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

// Capacity is in entries, not visual rows. On tiny terminals sacrifice chrome
// before the chosen entry; below three usable rows its block is clipped.
inline int listContentTop(int rows) {
    return rows >= 7 ? 2 : rows >= 5 ? 1 : 0;
}
inline int listContentEnd(int rows) {
    return rows >= 5 ? rows - 2 : rows >= 4 ? rows - 1 : std::max(0, rows);
}
inline int listCapacity(int rows) {
    const int available = listContentEnd(rows) - listContentTop(rows);
    return available > 0 ? std::max(1, available / 3) : 0;
}
inline std::string outputAge(const ListEntry &entry) {
    if (!entry.outputAgeMs)
        return "output unknown";
    const auto seconds = *entry.outputAgeMs / 1000;
    if (seconds == 0)
        return "output <1s ago";
    if (seconds < 60)
        return "output " + std::to_string(seconds) + "s ago";
    if (seconds < 3600)
        return "output " + std::to_string(seconds / 60) + "m ago";
    return "output " + std::to_string(seconds / 3600) + "h ago";
}

// Metadata previews must already be plain text. Fail closed if the daemon
// sends controls: merely removing ESC can reveal an OSC/DCS payload. This is
// validation, not another VT parser; the existing daemon parser supplies text.
inline std::string previewText(const ListEntry &entry, bool enabled) {
    if (!enabled)
        return "[preview disabled]";
    if (!entry.preview)
        return "[preview unavailable]";
    if (entry.preview->empty())
        return "[no recent output]";
    const std::string bounded = entry.preview->substr(0, 4096);
    for (const auto cp : ::tty::decodeUtf8(bounded)) {
        if (unsafeScreenCodePoint(cp))
            return "[unsafe preview omitted]";
    }
    return bounded;
}

// Draws the list over the whole canvas.
inline void renderList(const ListModel &model, ::tty::Canvas &canvas) {
    const int columns = canvas.columns();
    const int rows = canvas.rows();
    canvas.clear(kScreenBg);
    if (columns <= 0 || rows <= 0) {
        return;
    }
    if (rows >= 5) {
        fillRow(canvas, 0, kBarFg, kBarBg);
        put(canvas, 1, 0, "matterm: running terminals", kBarFg, kBarBg, true);
    }
    const auto &entries = model.entries();
    const int capacity = listCapacity(rows);
    const std::size_t first =
        capacity > 0 && model.index() >= std::size_t(capacity)
            ? model.index() + 1 - std::size_t(capacity)
            : 0;
    const int contentEnd = listContentEnd(rows);
    for (int slot = 0;
         slot < capacity && first + std::size_t(slot) < model.rowCount();
         ++slot) {
        const std::size_t i = first + std::size_t(slot);
        const bool chosen = i == model.index();
        const int y = listContentTop(rows) + slot * 3;
        const int height = std::min(3, contentEnd - y);
        for (int line = 0; line < height; ++line)
            fillRow(canvas, y + line, kScreenFg, kScreenBg);
        put(canvas, 0, y, chosen ? ">" : " ");
        if (i == 0) {
            int x = put(canvas, 2, y, "n ", kDim);
            put(canvas, x, y, "New terminal", kScreenFg, kScreenBg, true);
            if (height > 1)
                put(canvas,
                    4,
                    y + 1,
                    entries.empty() ? "No terminals are running in the daemon."
                                    : "Start a terminal with your shell.",
                    kDim);
        }
        else {
            const ListEntry &entry = entries[i - 1];
            const std::string number = i < 10 ? std::to_string(i) : " ";
            int x = put(canvas, 2, y, number + " ", kDim);
            x = put(canvas, x, y, entry.title, kScreenFg, kScreenBg, true);
            if (entry.thisTerminal)
                x = put(canvas, x, y, " (this terminal)", kWarn);
            else if (entry.inUse)
                x = put(canvas, x, y, " (in use)", kWarn);
            put(canvas,
                x,
                y,
                "  " + (entry.what.empty() ? "program unknown" : entry.what));
            if (height > 1) {
                x = put(canvas,
                        4,
                        y + 1,
                        entry.folder.empty() ? "folder unknown" : entry.folder,
                        kDim,
                        kScreenBg,
                        false,
                        std::max(8, (columns - 4) / 3));
                const std::string foreground =
                    !entry.foregroundBusy   ? "foreground unknown"
                    : *entry.foregroundBusy ? "foreground program"
                                            : "shell foreground";
                put(canvas,
                    x,
                    y + 1,
                    " | " + foreground + " | " +
                        (entry.inUse ? "attached" : "not attached") + " | " +
                        outputAge(entry),
                    kDim);
            }
            if (height > 2) {
                put(canvas,
                    4,
                    y + 2,
                    previewText(entry, model.previewEnabled()),
                    kDim);
            }
        }
        if (chosen) {
            // Inverse is a monochrome cue as well. Store the colours reversed
            // so the terminal's effective background is the light selection.
            // The literal > remains sufficient if all SGR is discarded.
            for (int line = 0; line < height; ++line) {
                for (int x = 0; x < columns; ++x) {
                    auto cell = canvas.at(x, y + line);
                    cell.foreground = kBarBg;
                    cell.background = kBarFg;
                    cell.inverse = true;
                    canvas.put(x, y + line, cell);
                }
            }
        }
    }
    const ListEntry *chosenEntry = model.selected();
    if (rows >= 5 && model.message().empty() && chosenEntry &&
        chosenEntry->inUse && !chosenEntry->thisTerminal) {
        put(canvas, 1, rows - 2, "In use: opening takes it over.", kWarn);
    }
    if (rows >= 5 && !model.message().empty()) {
        put(canvas, 1, rows - 2, model.message(), kWarn);
    }
    if (model.confirming()) {
        const int note = rows >= 4 ? rows - 2 : rows - 1;
        fillRow(canvas, note, kBarFg, kWarn);
        put(canvas, 0, note, ">", kBarFg, kWarn);
        put(canvas, 1, note, model.question(), kBarFg, kWarn, true);
    }
    if (rows >= 4) {
        fillRow(canvas, rows - 1, kBarFg, kBarBg);
        put(canvas,
            1,
            rows - 1,
            "Up/Down choose  Enter open  n new  x end  r refresh  q quit",
            kBarFg,
            kBarBg);
    }
}

} // namespace frontend::ttymode
