#pragma once

// Keys to layout commands (docs/TABS_PLAN.md section 3): the prefix state machine of
// the browser (and optionally the desktop app) and the desktop shortcut table. No SDL:
// the frontend turns its key events into KeyChord and acts on the result.
//
// `KeyChord::key` is the character the key produces (so '%' for Shift+5 and '"' for
// Shift+2 on the layouts that have them) or one of `keys::` for keys without one.
// Letters compare case-insensitively. Events for modifier keys alone must not be fed.
#include "tabtypes.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace frontend {

namespace keys {
enum Named : int {
    Up = 0x200000,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Escape,
    Enter,
    Tab,
    Backspace,
};
} // namespace keys

struct KeyChord {
    int key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

inline int foldKey(int key) {
    return key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key;
}

// A layout command with its argument (the tab index of SelectTab).
struct CommandCall {
    Command command = Command::NewTab;
    int argument = 0;
};

// ---- the prefix state machine ----

struct PrefixBinding {
    int key = 0;
    bool ctrl = false; // the second key is pressed with Ctrl (shift is ignored)
    CommandCall call;
};

// tmux-like defaults chosen for this project, see docs/tasks/tabs.md.
inline std::vector<PrefixBinding> defaultPrefixBindings() {
    std::vector<PrefixBinding> b;
    auto add = [&b](int key, Command c, int arg = 0) { b.push_back({key, false, {c, arg}}); };
    add('c', Command::NewTab);
    add('n', Command::NextTab);
    add('p', Command::PreviousTab);
    for (int d = 0; d <= 9; ++d) add('0' + d, Command::SelectTab, d);
    add('%', Command::SplitSideBySide);
    add('"', Command::SplitStacked);
    add('x', Command::ClosePane);
    add('&', Command::CloseTab);
    add(keys::Left, Command::FocusLeft);
    add(keys::Right, Command::FocusRight);
    add(keys::Up, Command::FocusUp);
    add(keys::Down, Command::FocusDown);
    add('h', Command::FocusLeft);
    add('l', Command::FocusRight);
    add('k', Command::FocusUp);
    add('j', Command::FocusDown);
    add('=', Command::EvenOut);
    return b;
}

// What the frontend does with one key event.
struct KeyOutcome {
    std::optional<CommandCall> command; // run this command
    bool forwardPrefix = false;         // the program gets the prefix key first
    bool forwardKey = false;            // and then the key that was just fed
    bool swallowed() const { return !command && !forwardPrefix && !forwardKey; }
};

class PrefixKeys {
public:
    struct Config {
        KeyChord prefix{'b', true, false, false};
        std::int64_t timeoutMs = 2000;
        std::vector<PrefixBinding> bindings = defaultPrefixBindings();
    };

    PrefixKeys() : PrefixKeys(Config{}) {}
    explicit PrefixKeys(Config config) : config_(std::move(config)) {}

    bool waiting() const { return waiting_; }
    // When tick() will have something to report, for the event loop's wait time.
    std::optional<std::int64_t> deadline() const {
        if (!waiting_) return std::nullopt;
        return deadline_;
    }
    void reset() { waiting_ = false; }

    // The prefix timed out: the program gets the key it never saw.
    KeyOutcome tick(std::int64_t nowMs) {
        KeyOutcome out;
        if (waiting_ && nowMs >= deadline_) {
            waiting_ = false;
            out.forwardPrefix = true;
        }
        return out;
    }

    // Feed a key press (not releases or repeats of modifier keys).
    //   idle, other key       -> forwardKey
    //   idle, the prefix      -> swallowed, now waiting
    //   waiting, bound key    -> the command
    //   waiting, the prefix   -> forwardPrefix only (a literal prefix key)
    //   waiting, other key    -> forwardPrefix and forwardKey
    // If the wait had timed out, its forwardPrefix is added first.
    KeyOutcome feed(const KeyChord &key, std::int64_t nowMs) {
        KeyOutcome out = tick(nowMs);
        if (!waiting_) {
            if (isPrefix(key)) {
                waiting_ = true;
                deadline_ = nowMs + config_.timeoutMs;
            }
            else {
                out.forwardKey = true;
            }
            return out;
        }
        waiting_ = false;
        if (isPrefix(key)) {
            out.forwardPrefix = true;
            return out;
        }
        for (const auto &b : config_.bindings) {
            if (foldKey(b.key) == foldKey(key.key) && b.ctrl == key.ctrl && !key.alt) {
                out.command = b.call;
                return out;
            }
        }
        out.forwardPrefix = true;
        out.forwardKey = true;
        return out;
    }

private:
    bool isPrefix(const KeyChord &k) const {
        return foldKey(k.key) == foldKey(config_.prefix.key) && k.ctrl == config_.prefix.ctrl &&
               k.alt == config_.prefix.alt;
    }

    Config config_;
    bool waiting_ = false;
    std::int64_t deadline_ = 0;
};

// ---- desktop shortcuts (GNOME Terminal style, plus own keys for panes) ----

enum class DesktopAction { Layout, Copy, Paste };

struct DesktopShortcut {
    KeyChord chord;
    DesktopAction action = DesktopAction::Layout;
    CommandCall call; // for Layout
};

inline const std::vector<DesktopShortcut> &desktopShortcuts() {
    static const std::vector<DesktopShortcut> table = [] {
        std::vector<DesktopShortcut> t;
        auto layout = [&t](KeyChord k, Command c, int arg = 0) {
            t.push_back({k, DesktopAction::Layout, {c, arg}});
        };
        constexpr bool yes = true;
        // GNOME Terminal.
        layout({'t', yes, yes, false}, Command::NewTab);
        layout({'w', yes, yes, false}, Command::CloseTab);
        layout({keys::PageDown, yes, false, false}, Command::NextTab);
        layout({keys::PageUp, yes, false, false}, Command::PreviousTab);
        for (int d = 1; d <= 9; ++d) layout({'0' + d, false, false, yes}, Command::SelectTab, d - 1);
        t.push_back({{'c', yes, yes, false}, DesktopAction::Copy, {}});
        t.push_back({{'v', yes, yes, false}, DesktopAction::Paste, {}});
        // Panes (Terminator and Windows Terminal flavoured; GNOME Terminal has none).
        layout({'e', yes, yes, false}, Command::SplitSideBySide);
        layout({'o', yes, yes, false}, Command::SplitStacked);
        layout({'w', false, yes, yes}, Command::ClosePane);
        layout({keys::Left, false, yes, yes}, Command::FocusLeft);
        layout({keys::Right, false, yes, yes}, Command::FocusRight);
        layout({keys::Up, false, yes, yes}, Command::FocusUp);
        layout({keys::Down, false, yes, yes}, Command::FocusDown);
        layout({'e', false, yes, yes}, Command::EvenOut);
        return t;
    }();
    return table;
}

// Exact match on the modifiers (Ctrl+PgUp is not Ctrl+Shift+PgUp).
inline std::optional<DesktopShortcut> findDesktopShortcut(const KeyChord &key) {
    for (const auto &s : desktopShortcuts()) {
        if (foldKey(s.chord.key) == foldKey(key.key) && s.chord.ctrl == key.ctrl &&
            s.chord.shift == key.shift && s.chord.alt == key.alt) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace frontend
