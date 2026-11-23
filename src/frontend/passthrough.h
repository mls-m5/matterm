#pragma once

// matmux passthrough (docs/tasks/matmux-passthrough.md): the program's output
// goes to the host terminal unchanged, so the host keeps its own scrollback,
// selection and mouse. This is the part without I/O: the attach step (the
// daemon's replay goes into a hidden model, then one snapshot is written), the
// live pass-through with a limit on what waits for the host, the input and the
// Ctrl+B prefix, and how it ends. The app does the real reads and writes, the
// raw mode and the restore on every way out.
//
// Contract with the caller: write hostOut() fully (the program's last words
// included) before acting on outcome(); the host terminal is put back
// (restore()) on every exit.
#include "terminalview.h"
#include "ttyattach.h"
#include "ttyinput.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frontend::passthrough {

using ttymode::AttachedPane;
using ttymode::kLeaveByte;
using ttymode::kPrefixByte;

struct Outcome {
    enum class Kind {
        Leave,     // Ctrl+B d: the client ends, the terminal keeps running.
        List,      // Ctrl+B l (or Ctrl+]): back to the list.
        New,       // Ctrl+B c: a new terminal.
        Ended,     // The program ended (exitCode).
        TakenOver, // Another client took the terminal over.
        Lost,      // The connection was lost.
        Gone,      // The daemon does not know the terminal (any more).
    };
    Kind kind = Kind::Leave;
    int exitCode = 0;
    std::string message; // A plain line for the user, no newline.
};

class Session {
public:
    struct Options {
        // What puts a host terminal into the state of the model (agent2's
        // tty::snapshot) and takes it back out of it (tty::hostRestore).
        std::function<std::string(const TerminalView &)> snapshot;
        std::function<std::string(bool inAlternateScreen)> restore;
        std::string title; // "bash" ... for the lines.
        std::string
            attachLine; // Printed before the snapshot (the keys), may be empty.
        std::size_t highWater =
            1u
            << 20; // Bytes waiting for the host before the daemon is not read.
        std::int64_t replayTimeoutMs =
            5000; // Show the screen anyway after this long.
        // A new terminal's startup queries must reach the host unchanged.
        bool fresh = false;
    };

    Session(AttachedPane &pane, Options options, std::int64_t now)
        : pane_(pane)
        , options_(std::move(options))
        , started_(now) {
        pane_.view().setOutputTap([this](std::string_view bytes) {
            if (live_) {
                out_.append(bytes);
            }
        });
        if (options_.fresh) {
            live_ = true;
            if (!options_.attachLine.empty()) {
                out_ += options_.attachLine + "\r\n";
            }
        }
    }
    ~Session() {
        pane_.view().setOutputTap({});
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Serve the connection; after the replay, write the snapshot.
    void tick(std::int64_t now) {
        if (outcome_) {
            return;
        }
        if (!stalled()) {
            pane_.tick();
        }
        flushInput(); // Retry input queued before the connection was running.
        if (!live_) {
            if (pane_.view().state() != TerminalView::State::Live) {
                live_ = true; // It ended or went away before there was anything
                              // to show.
            }
            else if (pane_.replayDone() ||
                     now - started_ >= options_.replayTimeoutMs) {
                goLive();
            }
        }
        checkEnd();
    }
    // Bytes of the host terminal's input, as they came.
    void input(std::string_view bytes) {
        for (const char c : bytes) {
            if (prefix_) {
                prefix_ = false;
                command(c);
            }
            else if (c == kPrefixByte) {
                prefix_ = true;
            }
            else if (c == kLeaveByte) {
                finish(Outcome::Kind::List, 0, "");
            }
            else {
                forward_.push_back(c);
            }
            if (outcome_) {
                break;
            }
        }
        flushInput();
    }
    // The host terminal changed size: the program redraws itself.
    void resized(int columns, int rows) {
        pane_.view().resize(std::size_t(std::max(1, columns)),
                            std::size_t(std::max(1, rows)));
    }

    // What waits for the host terminal. The caller writes from the front and
    // calls consumed(n).
    const std::string &hostOut() const {
        return out_;
    }
    void consumed(std::size_t n) {
        out_.erase(0, std::min(n, out_.size()));
    }
    // False while too much waits for the host: the daemon is not read then (so
    // nothing is dropped and the daemon is held back).
    bool wantsRead() const {
        return !stalled();
    }
    bool live() const {
        return live_;
    }
    const std::optional<Outcome> &outcome() const {
        return outcome_;
    }
    // What puts the host terminal back (the program may have left it in the
    // alternate screen, with the mouse on ...); the same on every exit.
    std::string restore() const {
        return options_.restore
                   ? options_.restore(pane_.view().model().alternateScreen())
                   : std::string();
    }

private:
    bool stalled() const {
        return live_ && out_.size() >= options_.highWater;
    }

    void goLive() {
        live_ = true;
        if (!options_.attachLine.empty()) {
            out_ += options_.attachLine;
            out_ += "\r\n";
        }
        if (options_.snapshot) {
            out_ += options_.snapshot(pane_.view());
        }
        flushInput();
    }

    void command(char c) {
        switch (c) {
        case 'd':
            finish(Outcome::Kind::Leave, 0, "");
            break;
        case 'l':
            finish(Outcome::Kind::List, 0, "");
            break;
        case 'c':
            finish(Outcome::Kind::New, 0, "");
            break;
        case kPrefixByte:
            forward_.push_back(
                kPrefixByte); // Twice: one Ctrl+B for the program.
            break;
        default:
            break; // Anything else only cancels the prefix.
        }
    }

    void flushInput() {
        if (live_ && !forward_.empty() &&
            pane_.view().state() == TerminalView::State::Live) {
            if (pane_.view().sendInput(forward_)) {
                forward_.clear();
            }
        }
    }

    void finish(Outcome::Kind kind, int code, std::string message) {
        if (!outcome_) {
            outcome_ = Outcome{kind, code, std::move(message)};
        }
    }

    void checkEnd() {
        const TerminalView &view = pane_.view();
        const std::string what =
            options_.title.empty() ? "the terminal" : options_.title;
        switch (view.state()) {
        case TerminalView::State::Live:
            break;
        case TerminalView::State::Exited:
            finish(Outcome::Kind::Ended,
                   view.exitSignal() ? 128 + view.exitSignal()
                                     : view.exitCode(),
                   "The program in " + what + " ended (" +
                       (view.exitSignal()
                            ? "signal " + std::to_string(view.exitSignal())
                            : "exit code " + std::to_string(view.exitCode())) +
                       ")");
            break;
        case TerminalView::State::Replaced:
            finish(Outcome::Kind::TakenOver,
                   0,
                   what + " was taken over by another client");
            break;
        case TerminalView::State::Disconnected:
            finish(Outcome::Kind::Lost,
                   0,
                   "The connection to " + what + " was lost");
            break;
        case TerminalView::State::Unavailable:
            finish(Outcome::Kind::Gone, 0, what + " is gone");
            break;
        }
    }

    AttachedPane &pane_;
    Options options_;
    std::int64_t started_ = 0;
    bool live_ = false;
    bool prefix_ = false;
    std::string out_;
    std::string forward_; // Input waiting for the attach to finish.
    std::optional<Outcome> outcome_;
};

} // namespace frontend::passthrough
