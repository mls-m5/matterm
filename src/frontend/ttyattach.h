#pragma once

// `matterm --attach-tty` (docs/tasks/attach-tty.md): the mode itself. A list of
// the terminals that run in the daemon, one of them full screen in the host
// terminal, the keys of the host terminal forwarded to it. This class knows
// nothing of sockets, termios or escape sequences for drawing: it works against
// three small interfaces (the host screen, where the terminals come from, the
// open terminal), so that it is tested with fakes and the real parts are
// plugged in by the app.
//
// Time is a number of milliseconds on any steady clock; the caller passes it
// in.
#include "terminalview.h"
#include "tty/matterm/cells.h"
#include "tty/screen.h"
#include "ttyinput.h"
#include "ttylist.h"
#include "ttyscreen.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frontend::ttymode {

// The host terminal as the mode needs it: its size, a picture to show, which
// reports to ask for. Implemented by the app (a plain one now, the shared
// src/tty layer later).
class HostScreen {
public:
    virtual ~HostScreen() = default;
    // The screen to draw into (size included); the mode calls present() after
    // drawing.
    virtual ::tty::Screen &screen() = 0;
    virtual void present() = 0;
    virtual void setInputModes(const HostInputModes &modes) = 0;
};

// One terminal of the daemon that this client has taken over.
class AttachedPane {
public:
    virtual ~AttachedPane() = default;
    virtual TerminalView &view() = 0;
    // Serves the connection (acks, pings, incoming output). Call often; cheap
    // when idle.
    virtual void tick() = 0;
    // Attaches again after the view lost its connection or was taken over: this
    // takes the terminal back. Returns false when it could not even start.
    virtual bool reattach() = 0;
    // The connection's descriptor to wait on (-1: none), and when tick() is due
    // next.
    virtual int pollFd() const = 0;
    virtual bool wantsWrite() const {
        return false;
    }
    virtual std::optional<std::int64_t> nextTickMs() const = 0;
};

class SessionSource {
public:
    virtual ~SessionSource() = default;
    // The running terminals; on failure an empty list and `error` set.
    virtual std::vector<ListEntry> list(std::string &error) = 0;
    // Takes the terminal over, sized `columns` x `rows`. Null with `error` when
    // it cannot.
    virtual std::unique_ptr<AttachedPane> open(const std::string &id,
                                               int columns,
                                               int rows,
                                               std::string &error) = 0;
};

class AttachTty {
public:
    struct Options {
        std::int64_t hintMs = 8000; // How long the first-time hint stays.
        std::int64_t escapeMs =
            30; // How long a lone Escape waits for the rest of a sequence.
    };

    AttachTty(HostScreen &host, SessionSource &source)
        : host_(host)
        , source_(source) {}
    AttachTty(HostScreen &host, SessionSource &source, Options options)
        : host_(host)
        , source_(source)
        , options_(options) {}

    void start(std::int64_t now) {
        now_ = now;
        refresh();
        dirty_ = true;
        draw();
    }

    // Bytes the user typed.
    void input(std::string_view bytes, std::int64_t now) {
        now_ = now;
        handle(decoder_.feed(bytes));
        draw();
    }
    // The input went quiet: a waiting Escape is the Escape key.
    void quiet(std::int64_t now) {
        now_ = now;
        if (decoder_.pending()) {
            handle(decoder_.flush());
        }
        draw();
    }
    void resized(std::int64_t now) {
        now_ = now;
        if (pane_) {
            const auto [columns, rows] = size();
            pane_->view().resize(std::size_t(std::max(1, columns)),
                                 std::size_t(std::max(1, rows)));
        }
        dirty_ = true;
        draw();
    }
    // Serve the connection and show what changed.
    void tick(std::int64_t now) {
        now_ = now;
        if (pane_) {
            pane_->tick();
            if (hintUntil_ && now_ >= hintUntil_) {
                hintUntil_ = 0;
                dirty_ = true;
            }
        }
        draw();
    }

    bool finished() const {
        return finished_;
    }
    bool inPane() const {
        return pane_ != nullptr;
    }
    const ListModel &list() const {
        return list_;
    }
    // What to wait on: the pane's connection and the host input; `timeoutMs` -1
    // means no deadline.
    int pollFd() const {
        return pane_ ? pane_->pollFd() : -1;
    }
    bool paneWantsWrite() const {
        return pane_ && pane_->wantsWrite();
    }
    int timeoutMs(std::int64_t now) const {
        std::int64_t wait = -1;
        const auto soonest = [&](std::int64_t at) {
            const std::int64_t left = std::max<std::int64_t>(0, at - now);
            wait = wait < 0 ? left : std::min(wait, left);
        };
        if (decoder_.pending()) {
            soonest(now + options_.escapeMs);
        }
        if (pane_) {
            if (const auto due = pane_->nextTickMs()) {
                soonest(*due);
            }
            if (hintUntil_) {
                soonest(hintUntil_);
            }
        }
        return int(std::min<std::int64_t>(wait, 3600 * 1000));
    }

private:
    bool live() const {
        return pane_ && pane_->view().state() == TerminalView::State::Live;
    }

    void refresh() {
        std::string error;
        auto entries = source_.list(error);
        list_.setEntries(std::move(entries));
        list_.setMessage(error);
        dirty_ = true;
    }

    void handle(const std::vector<InputEvent> &events) {
        for (const InputEvent &event : events) {
            if (finished_) {
                return;
            }
            if (pane_) {
                paneEvent(event);
            }
            else {
                listEvent(event);
            }
        }
    }

    void listEvent(const InputEvent &event) {
        // A run of typed characters is one Bytes event: each is a keystroke of
        // its own here.
        if (event.kind == InputEvent::Kind::Bytes && event.text.size() > 1) {
            for (const char c : event.text) {
                InputEvent single = event;
                single.text.assign(1, c);
                listEvent(single);
                if (pane_ || finished_) {
                    return;
                }
            }
            return;
        }
        int digit = 0;
        const ListKey key = listKeyOf(event, digit);
        if (key == ListKey::None) {
            return;
        }
        const auto [columns, rows] = size();
        const ListAction action =
            list_.key(key, digit, std::max(1, listCapacity(rows)));
        (void)columns;
        dirty_ = true;
        switch (action.kind) {
        case ListAction::Kind::Quit:
            finished_ = true;
            break;
        case ListAction::Kind::Refresh:
            refresh();
            break;
        case ListAction::Kind::Open:
            open(action.id);
            break;
        case ListAction::Kind::None:
            break;
        }
    }

    void open(const std::string &id) {
        const auto [columns, rows] = size();
        std::string error;
        auto pane =
            source_.open(id, std::max(1, columns), std::max(1, rows), error);
        if (!pane) {
            list_.setMessage(error.empty() ? "That terminal could not be opened"
                                           : error);
            return;
        }
        list_.setMessage({});
        pane_ = std::move(pane);
        pane_->view().setChanged([this] { dirty_ = true; });
        // The first time in this run the leave key is announced; later the user
        // knows it.
        if (!hinted_) {
            hinted_ = true;
            hintUntil_ = now_ + options_.hintMs;
        }
        dirty_ = true;
    }

    void leavePane() {
        const std::string title = list_.selected()
                                      ? list_.selected()->title
                                      : std::string("the terminal");
        pane_.reset(); // The terminal keeps running in the daemon; only this
                       // connection ends.
        hintUntil_ = 0;
        applyModes();
        refresh();
        list_.setMessage("Left " + title + "; it keeps running");
        dirty_ = true;
    }

    void paneEvent(const InputEvent &event) {
        if (event.kind == InputEvent::Kind::Leave) {
            leavePane();
            return;
        }
        TerminalView &view = pane_->view();
        if (view.state() == TerminalView::State::Live) {
            if (hintUntil_) { // The user knows what to do: the hint has done
                              // its job.
                hintUntil_ = 0;
                dirty_ = true;
            }
            const std::string bytes = encodeForProgram(event, view.modes());
            if (!bytes.empty()) {
                view.sendInput(bytes);
            }
            return;
        }
        // Not live: the only keys are Enter (take it back, or back to the list)
        // and the leave key.
        if (event.kind == InputEvent::Kind::Key &&
            event.key == InputEncoder::Key::Enter) {
            switch (view.state()) {
            case TerminalView::State::Replaced:
            case TerminalView::State::Disconnected:
                if (!pane_->reattach()) {
                    list_.setMessage("Could not connect again");
                }
                dirty_ = true;
                break;
            case TerminalView::State::Unavailable:
            case TerminalView::State::Exited:
                leavePane();
                break;
            case TerminalView::State::Live:
                break;
            }
        }
    }

    // The line at the bottom: the first text that fits the width (a phone has
    // about 40 columns).
    std::string statusText(int columns) const {
        if (!pane_) {
            return {};
        }
        const std::string leave = kLeaveName;
        std::vector<std::string> variants;
        switch (pane_->view().state()) {
        case TerminalView::State::Live:
            if (hintUntil_) {
                variants = {
                    leave + " leaves to the list; the terminal keeps running",
                    leave + " leaves to the list"};
            }
            break;
        case TerminalView::State::Replaced:
            variants = {
                "Taken over by another client.  Enter: take it back   " +
                    leave + ": list",
                "Taken over. Enter: take back, " + leave + ": list",
                "Taken over. Enter: take back"};
            break;
        case TerminalView::State::Disconnected:
            variants = {"Connection lost.  Enter: connect again   " + leave +
                            ": list",
                        "Connection lost. Enter: retry, " + leave + ": list",
                        "Connection lost. Enter: retry"};
            break;
        case TerminalView::State::Unavailable:
            variants = {"This terminal is gone.  Enter or " + leave + ": list",
                        "Gone. Enter: list"};
            break;
        case TerminalView::State::Exited:
            variants = {"The program ended.  Enter or " + leave + ": list",
                        "Ended. Enter: list"};
            break;
        }
        for (const std::string &text : variants) {
            if (int(text.size()) + 2 <= columns) {
                return text;
            }
        }
        return variants.empty() ? std::string() : variants.back();
    }

    void applyModes() {
        HostInputModes wanted;
        if (pane_) {
            wanted = hostModesFor(pane_->view().modes(), live());
        }
        if (!modesSet_ || wanted != modes_) {
            modes_ = wanted;
            modesSet_ = true;
            host_.setInputModes(wanted);
        }
    }

    std::pair<int, int> size() const {
        return {host_.screen().columns(), host_.screen().rows()};
    }

    void draw() {
        if (!dirty_) {
            return;
        }
        dirty_ = false;
        const auto [columns, rows] = size();
        ::tty::Screen &screen = host_.screen();
        ::tty::Canvas &canvas = screen.canvas();
        if (pane_) {
            TerminalView &view = pane_->view();
            const auto frame = view.model().frame(16);
            canvas.clear(kScreenBg);
            ::tty::drawCells(screen, frame.cells, view.palette());
            ::tty::Cursor cursor = ::tty::toCursor(frame.cursor);
            cursor.visible = cursor.visible && frame.cursor.column < columns &&
                             frame.cursor.row < rows;
            const std::string status = statusText(columns);
            if (!status.empty() && rows >= 2) {
                fillRow(canvas, rows - 1, kBarFg, kBarBg);
                put(canvas, 1, rows - 1, status, kBarFg, kBarBg);
                if (cursor.row == rows - 1) {
                    cursor.visible = false;
                }
            }
            screen.setCursor(cursor);
        }
        else {
            renderList(list_, canvas);
            screen.setCursor({});
        }
        applyModes();
        host_.present();
    }

    HostScreen &host_;
    SessionSource &source_;
    Options options_{};
    ListModel list_;
    HostInputDecoder decoder_;
    std::unique_ptr<AttachedPane> pane_;
    std::int64_t now_ = 0;
    std::int64_t hintUntil_ = 0;
    bool hinted_ = false;
    bool finished_ = false;
    bool dirty_ = true;
    bool modesSet_ = false;
    HostInputModes modes_;
};

} // namespace frontend::ttymode
