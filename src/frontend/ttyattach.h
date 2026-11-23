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
    // The daemon's replay of what the program wrote before this attach has all
    // arrived (the passthrough mode shows the screen once it has).
    virtual bool replayDone() const {
        return true;
    }
    virtual std::optional<std::int64_t> nextTickMs() const = 0;
};

// What a new terminal runs and is called; empty command: the user's shell.
struct NewTerminal {
    std::string name;
    std::vector<std::string> command;
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
    // Starts a terminal in the daemon (in the current folder) and returns its
    // id; empty with `error` set when it cannot.
    virtual std::string create(const NewTerminal &what,
                               int columns,
                               int rows,
                               std::string &error) = 0;
};

class AttachTty {
public:
    struct Options {
        std::int64_t hintMs =
            8000; // How long the first-time hint stays (no status line).
        std::int64_t escapeMs =
            30; // How long a lone Escape waits for the rest of a sequence.
        bool statusLine = true;  // The permanent line at the bottom (Ctrl+B s).
        int statusMinRows = 24;  // It starts off on a screen with fewer rows.
        bool startNew = false;   // Start a terminal at once (--new) instead of
                                 // showing the list.
        NewTerminal newTerminal; // What --new starts.
        bool exitWhenEnded = false; // --new: the client ends with the program.
        bool selectNew = false; // matmux without arguments: Enter in the list
                                // starts a new terminal.
        bool listOnly = false; // matmux: the list only chooses; the terminal is
                               // shown by the caller.
        std::string initialMessage; // A line under the list at the start (how
                                    // the last terminal ended).
        std::string comeBack = "matterm --attach-tty"; // What the farewell says
                                                       // to run to come back.
        std::string ownSession; // The terminal this program runs in
                                // (MATTERM_SESSION): never attached.
    };

    // What the list chose when it was started with listOnly.
    struct Choice {
        enum class Kind { None, Open, New } kind = Kind::None;
        std::string id;    // Open.
        std::string title; // Open: the name of the terminal.
    };

    AttachTty(HostScreen &host, SessionSource &source)
        : host_(host)
        , source_(source) {}
    AttachTty(HostScreen &host, SessionSource &source, Options options)
        : host_(host)
        , source_(source)
        , options_(std::move(options)) {}

    void start(std::int64_t now) {
        now_ = now;
        statusOn_ =
            options_.statusLine && size().second >= options_.statusMinRows;
        list_.setPreferNew(options_.selectNew);
        refresh();
        if (!options_.initialMessage.empty()) {
            list_.setMessage(options_.initialMessage);
        }
        if (options_.startNew) {
            createAndOpen(options_.newTerminal, "new terminal");
        }
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
        applyPaneSize();
        dirty_ = true;
        draw();
    }
    // Serve the connection and show what changed.
    void tick(std::int64_t now) {
        now_ = now;
        if (pane_) {
            pane_->tick();
            // A flood of output (a replay after attaching) is drawn at most ten
            // times a second: the model has it all, a picture per chunk only
            // costs time (docs/CPU.md "Replay").
            const auto ingested = pane_->view().outputBytes();
            flooded_ = ingested > ingestSeen_ && ingested - ingestSeen_ > 32768;
            ingestSeen_ = ingested;
            if (hintUntil_ && now_ >= hintUntil_) {
                hintUntil_ = 0;
                dirty_ = true;
            }
            if (pane_->view().state() == TerminalView::State::Exited) {
                programEnded();
            }
        }
        if (dirty_ && flooded_ && lastDrawAt_ != 0 &&
            now_ - lastDrawAt_ < kFloodDrawMs) {
            held_ = true; // timeoutMs() wakes us when it may be drawn.
            return;
        }
        held_ = false;
        draw();
    }

    bool finished() const {
        return finished_;
    }
    // A line for after the terminal is restored (what was left running, how the
    // program ended).
    const Choice &choice() const {
        return choice_;
    }
    const std::string &farewell() const {
        return farewell_;
    }
    // The exit code of the program when --new ended with it, else 0.
    int exitCode() const {
        return exitCode_;
    }
    bool inPane() const {
        return pane_ != nullptr;
    }
    bool statusLineOn() const {
        return statusOn_;
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
        if (held_) {
            soonest(lastDrawAt_ + kFloodDrawMs);
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
    std::pair<int, int> size() const {
        return {host_.screen().columns(), host_.screen().rows()};
    }
    // The program has the screen minus the status line.
    std::pair<int, int> paneSize() const {
        const auto [columns, rows] = size();
        return {std::max(1, columns),
                std::max(1, rows - (statusOn_ && rows >= 2 ? 1 : 0))};
    }
    void applyPaneSize() {
        if (pane_) {
            const auto [columns, rows] = paneSize();
            pane_->view().resize(std::size_t(columns), std::size_t(rows));
        }
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

    // A run of typed characters is one Bytes event: where each is a keystroke
    // of its own, this splits it.
    template <class F>
    bool eachKeystroke(const InputEvent &event, F &&again) {
        if (event.kind == InputEvent::Kind::Bytes && event.text.size() > 1) {
            for (const char c : event.text) {
                InputEvent single = event;
                single.text.assign(1, c);
                again(single);
                if (finished_) {
                    return true;
                }
            }
            return true;
        }
        return false;
    }

    void listEvent(const InputEvent &event) {
        if (eachKeystroke(event, [&](const InputEvent &single) {
                if (!pane_) {
                    listEvent(single);
                }
            })) {
            return;
        }
        int digit = 0;
        const ListKey key = listKeyOf(event, digit);
        if (key == ListKey::None) {
            return;
        }
        const ListAction action =
            list_.key(key, digit, std::max(1, listCapacity(size().second)));
        dirty_ = true;
        switch (action.kind) {
        case ListAction::Kind::Quit:
            finished_ = true;
            break;
        case ListAction::Kind::Refresh:
            refresh();
            break;
        case ListAction::Kind::Open:
            if (options_.listOnly) {
                choice_ = {Choice::Kind::Open,
                           action.id,
                           list_.selected() ? list_.selected()->title
                                            : std::string()};
                finished_ = true;
                break;
            }
            open(action.id,
                 list_.selected() ? list_.selected()->title : std::string());
            break;
        case ListAction::Kind::New:
            if (options_.listOnly) {
                choice_ = {Choice::Kind::New, {}, {}};
                finished_ = true;
                break;
            }
            createAndOpen({}, "new terminal");
            break;
        case ListAction::Kind::None:
            break;
        }
    }

    void open(const std::string &id, const std::string &title) {
        if (!options_.ownSession.empty() && id == options_.ownSession) {
            list_.setMessage("You are in this terminal: attaching it would "
                             "feed it to itself");
            return;
        }
        const auto [columns, rows] = paneSize();
        std::string error;
        auto pane = source_.open(id, columns, rows, error);
        if (!pane) {
            list_.setMessage(error.empty() ? "That terminal could not be opened"
                                           : error);
            return;
        }
        list_.setMessage({});
        pane_ = std::move(pane);
        paneTitle_ = title.empty() ? "terminal " + id.substr(0, 8) : title;
        prefixPending_ = helpShown_ = false;
        pane_->view().setChanged([this] { dirty_ = true; });
        // The connection says hello only when it is first served; waiting for
        // the next key (or for output that cannot come before the hello) would
        // hang a client that was started with --new.
        pane_->tick();
        // Without the status line the leave key is announced the first time in
        // this run.
        if (!hinted_ && !statusOn_) {
            hinted_ = true;
            hintUntil_ = now_ + options_.hintMs;
        }
        dirty_ = true;
    }

    void createAndOpen(const NewTerminal &what,
                       const std::string &fallbackTitle) {
        const auto [columns, rows] = paneSize();
        std::string error;
        const std::string id = source_.create(what, columns, rows, error);
        if (id.empty()) {
            list_.setMessage(error.empty()
                                 ? "The daemon could not start a terminal"
                                 : error);
            dirty_ = true;
            return;
        }
        open(id, what.name.empty() ? fallbackTitle : what.name);
    }

    void leavePane() {
        const std::string title = paneTitle_;
        pane_.reset(); // The terminal keeps running in the daemon; only this
                       // connection ends.
        hintUntil_ = 0;
        prefixPending_ = helpShown_ = false;
        applyModes();
        refresh();
        list_.setMessage("Left " + title + "; it keeps running");
        dirty_ = true;
    }

    // The program in the pane ended: say so, and back to the list (or out, for
    // --new).
    void programEnded() {
        const TerminalView &view = pane_->view();
        const std::string how =
            view.exitSignal() ? "signal " + std::to_string(view.exitSignal())
                              : "exit code " + std::to_string(view.exitCode());
        const std::string message =
            "The program in " + paneTitle_ + " ended (" + how + ")";
        if (options_.exitWhenEnded) {
            exitCode_ =
                view.exitSignal() ? 128 + view.exitSignal() : view.exitCode();
            farewell_ = message;
            finished_ = true;
            return;
        }
        pane_.reset();
        hintUntil_ = 0;
        prefixPending_ = helpShown_ = false;
        applyModes();
        refresh();
        list_.setMessage(message);
        dirty_ = true;
    }

    // Ctrl+B and the key after it.
    void prefixCommand(const InputEvent &event) {
        const bool help = helpShown_;
        prefixPending_ = helpShown_ = false;
        dirty_ = true;
        if (help) {
            return; // Any key closes the help.
        }
        if (event.kind ==
            InputEvent::Kind::Prefix) { // Twice: one Ctrl+B for the program.
            if (live()) {
                pane_->view().sendInput(std::string(1, kPrefixByte));
            }
            return;
        }
        if (event.kind == InputEvent::Kind::Leave) {
            leavePane();
            return;
        }
        if (event.kind != InputEvent::Kind::Bytes || event.text.empty()) {
            return; // Escape and the other keys only cancel.
        }
        switch (event.text[0]) {
        case 'd':
            farewell_ = paneTitle_ + " keeps running; " + options_.comeBack +
                        " comes back to it";
            pane_.reset();
            finished_ = true;
            break;
        case 'l':
            leavePane();
            break;
        case 'c':
            pane_.reset();
            hintUntil_ = 0;
            createAndOpen({}, "new terminal");
            if (!pane_) {
                refresh();
            }
            break;
        case 's':
            statusOn_ = !statusOn_;
            applyPaneSize();
            break;
        case '?':
            helpShown_ = prefixPending_ = true;
            break;
        default:
            break;
        }
    }

    void paneEvent(const InputEvent &event) {
        if (prefixPending_) {
            if (event.kind == InputEvent::Kind::Bytes &&
                event.text.size() > 1) {
                InputEvent first = event;
                first.text.assign(1, event.text[0]);
                prefixCommand(first);
                if (pane_ && !finished_) {
                    InputEvent rest = event;
                    rest.text.erase(0, 1);
                    paneEvent(rest);
                }
                return;
            }
            prefixCommand(event);
            return;
        }
        if (event.kind == InputEvent::Kind::Prefix) {
            prefixPending_ = true;
            dirty_ = true;
            return;
        }
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
        // and the prefix and leave keys.
        if (event.kind == InputEvent::Kind::Key &&
            event.key == InputEncoder::Key::Enter) {
            switch (view.state()) {
            case TerminalView::State::Replaced:
            case TerminalView::State::Disconnected:
                if (!pane_->reattach()) {
                    list_.setMessage("Could not connect again");
                }
                else {
                    pane_->tick(); // See open().
                }
                dirty_ = true;
                break;
            case TerminalView::State::Unavailable:
                leavePane();
                break;
            case TerminalView::State::Exited:
            case TerminalView::State::Live:
                break;
            }
        }
    }

    static std::string firstThatFits(const std::vector<std::string> &variants,
                                     int columns) {
        for (const std::string &text : variants) {
            if (int(text.size()) + 2 <= columns) {
                return text;
            }
        }
        return variants.empty() ? std::string() : variants.back();
    }

    // The line at the bottom: what the pane needs said now, else the status
    // line. The first text that fits the width (a phone has about 40 columns).
    std::string statusText(int columns) const {
        if (!pane_) {
            return {};
        }
        if (prefixPending_ && !helpShown_) {
            return firstThatFits({"Ctrl+B:  d leave   l list   c new   s "
                                  "status   ? help   Ctrl+B sends it",
                                  "d leave  l list  c new  s status  ? help",
                                  "d l c s ?"},
                                 columns);
        }
        const std::string leave = kLeaveName;
        switch (pane_->view().state()) {
        case TerminalView::State::Live:
            if (hintUntil_) {
                return firstThatFits(
                    {leave + " leaves to the list; the terminal keeps running",
                     leave + " leaves to the list"},
                    columns);
            }
            if (statusOn_) {
                return firstThatFits(
                    {paneTitle_ +
                         "   Ctrl+B: d leave  l list  c new  s status  ? help",
                     paneTitle_ + "   Ctrl+B ? help",
                     "Ctrl+B ? help"},
                    columns);
            }
            return {};
        case TerminalView::State::Replaced:
            return firstThatFits(
                {"Taken over by another client.  Enter: take it back   " +
                     leave + ": list",
                 "Taken over. Enter: take back, " + leave + ": list",
                 "Taken over. Enter: take back"},
                columns);
        case TerminalView::State::Disconnected:
            return firstThatFits(
                {"Connection lost.  Enter: connect again   " + leave + ": list",
                 "Connection lost. Enter: retry, " + leave + ": list",
                 "Connection lost. Enter: retry"},
                columns);
        case TerminalView::State::Unavailable:
            return firstThatFits(
                {"This terminal is gone.  Enter or " + leave + ": list",
                 "Gone. Enter: list"},
                columns);
        case TerminalView::State::Exited:
            return {}; // programEnded() has taken it away already.
        }
        return {};
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

    void drawHelp(::tty::Canvas &canvas, int columns, int rows) {
        static const char *const lines[] = {
            "Ctrl+B d   leave; the terminal keeps running",
            "Ctrl+B l   the list of terminals (Ctrl+] too)",
            "Ctrl+B c   a new terminal",
            "Ctrl+B s   the status line on or off",
            "Ctrl+B Ctrl+B   send Ctrl+B to the program",
            "any key closes this",
        };
        const int count = int(sizeof lines / sizeof *lines);
        for (int i = 0; i < count && rows - count + i >= 0; ++i) {
            const int y = rows - count + i;
            fillRow(canvas, y, kBarFg, kBarBg);
            put(canvas, 1, y, lines[i], kBarFg, kBarBg);
        }
        (void)columns;
    }

    void draw() {
        if (!dirty_) {
            return;
        }
        dirty_ = false;
        held_ = false;
        lastDrawAt_ = now_ == 0 ? 1 : now_;
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
            if (helpShown_ && rows >= 8) {
                drawHelp(canvas, columns, rows);
                cursor.visible = false;
            }
            else if (!status.empty() && rows >= 2) {
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
    std::string paneTitle_;
    std::string farewell_;
    Choice choice_;
    int exitCode_ = 0;
    std::int64_t now_ = 0;
    std::int64_t hintUntil_ = 0;
    bool hinted_ = false;
    bool finished_ = false;
    static constexpr std::int64_t kFloodDrawMs = 100;
    std::uint64_t ingestSeen_ = 0;
    std::int64_t lastDrawAt_ = 0;
    bool flooded_ = false;
    bool held_ = false;
    bool dirty_ = true;
    bool modesSet_ = false;
    bool statusOn_ = false;
    bool prefixPending_ = false;
    bool helpShown_ = false;
    HostInputModes modes_;
};

} // namespace frontend::ttymode
