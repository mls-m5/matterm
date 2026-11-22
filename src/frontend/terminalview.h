#pragma once

#include "clientsession.h"
#include "cursorglide.h"
#include "palette.h"
#include "terminallink.h"
#include "terminalmodel.h"
#include "textselection.h"
#include "vtparser.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frontend {
struct TerminalViewConfig {
    std::size_t columns = 80;
    std::size_t rows = 24;
    std::size_t scrollback = 1000;
    CursorGlide::Config glide;
    // Scrolling by wheel or finger stops on whole rows once it pauses. Off: the view may rest
    // in the middle of a row (pixel scrolling).
    bool snapScrollToRows = false;
    // The line written into the terminal when another window takes the session over.
    std::string replacedNotice = "session taken over by another window";
};

// One terminal in a window: its own model, parser, palette, title, selection
// and cursor glide, fed by one TerminalLink. Nothing here draws or reads input
// devices, so a window can hold several views, parse and acknowledge the ones
// that are not shown, and a native front end can use the same class.
//
// The view reports how its link ended (process exit, taken over by another
// window, session unavailable, connection lost) and writes a short notice into
// the terminal. It never reconnects by itself: the owner attaches a new link
// when the user asks.
//
// The link is not owned and must outlive the view (declare it first). Call
// everything from the thread that drives the link.
class TerminalView {
public:
    using Config = TerminalViewConfig;
    enum class State {
        Live,        // Attached, or waiting for the connection.
        Exited,      // The process ended.
        Replaced,    // Another window took the session over.
        Unavailable, // The server does not know the session id (any more).
        Disconnected // The connection was lost or the server refused it.
    };
    // The protocol error that ended a ClientSession, if any. Lets the view tell
    // a take-over from a network failure without reading human text.
    using ErrorSource = std::function<std::optional<protocol::ErrorCode>()>;

    explicit TerminalView(Config config = {})
        : config_(config)
        , glide_(config.glide) {
        build(config.columns, config.rows);
    }
    ~TerminalView() {
        detach();
    }
    TerminalView(const TerminalView &) = delete;
    TerminalView &operator=(const TerminalView &) = delete;

    // Listens to `link` from now on and treats the view as live again. An
    // earlier link is let go first.
    void attach(TerminalLink &link, ErrorSource errorCode = {}) {
        detach();
        link_ = &link;
        errorCode_ = std::move(errorCode);
        state_ = State::Live;
        exitCode_ = exitSignal_ = 0;
        TerminalLink::Events events;
        events.output = [this](std::string_view bytes) {
            parser_->feed(bytes);
            changed();
        };
        events.exited = [this](int code, int signal) {
            onExited(code, signal);
        };
        events.closed = [this](std::string_view reason) { onClosed(reason); };
        link.setEvents(std::move(events));
    }
    void detach() {
        if (link_) {
            link_->setEvents({});
            link_ = nullptr;
        }
    }

    // Fills the per-view parts of a session config: size, replay negotiation,
    // the id to attach to (empty: a new terminal), the offset to continue from
    // (0 after a reload, or the retained offset when this view keeps its parsed
    // state) and the reset that a full replay needs.
    void configure(ClientSession::Config &config,
                   std::string sessionId,
                   std::uint64_t resumeOffset = 0) {
        config.columns = int(model_->columns());
        config.rows = int(model_->rows());
        config.replay = true;
        config.sessionId = std::move(sessionId);
        config.resumeOffset = resumeOffset;
        config.resetForReplay = [this] { resetForReplay(); };
    }

    // Input goes to the program only while the view is live.
    bool sendInput(std::string_view bytes) {
        return link_ && state_ == State::Live && link_->sendInput(bytes);
    }
    void resize(std::size_t columns, std::size_t rows) {
        model_->resize(columns, rows);
        if (link_ && state_ == State::Live) {
            link_->resize(int(columns), int(rows));
        }
    }

    // The owner could not create a link at all: the same as losing one.
    void linkFailed(std::string_view reason) {
        onClosed(reason);
    }
    // A line for the user in the terminal, such as what a key will do now.
    void note(std::string_view text) {
        notice(std::string(text));
    }

    // Forgets everything that was parsed: screen, history, modes, palette,
    // title, selection. The model object is replaced, so references to it
    // (`model()`) must be fetched again.
    void resetForReplay() {
        const auto columns = model_->columns();
        const auto rows = model_->rows();
        selection.reset();
        palette_ = defaultPalette_;
        title_.clear();
        glide_ = CursorGlide(config_.glide);
        build(columns, rows);
        changed();
    }

    TerminalModel &model() {
        return *model_;
    }
    const TerminalModel &model() const {
        return *model_;
    }
    // What the program running in this view has asked for (mouse reporting,
    // bracketed paste, application keys, ...).
    const TerminalModes &modes() const {
        return parser_->modes();
    }
    const Palette &palette() const {
        return palette_;
    }
    CursorGlide &glide() {
        return glide_;
    }
    const std::string &title() const {
        return title_;
    }
    State state() const {
        return state_;
    }
    bool ended() const {
        return state_ != State::Live;
    }
    // Only a lost connection may be retried; taking a session back from
    // another window, or replacing a missing one, are the user's decisions.
    bool reconnectAllowed() const {
        return state_ == State::Disconnected;
    }
    int exitCode() const {
        return exitCode_;
    }
    int exitSignal() const {
        return exitSignal_;
    }
    // Called after output, a title change or the end of the session, also
    // while the view is not shown.
    void setChanged(std::function<void()> changed) {
        changed_ = std::move(changed);
    }

    std::optional<Selection> selection;

private:
    void build(std::size_t columns, std::size_t rows) {
        parser_.reset(); // Borrows the model.
        model_ =
            std::make_unique<TerminalModel>(columns, rows, config_.scrollback);
        model_->setSnapScrollToRows(config_.snapScrollToRows);
        VtEvents events;
        events.reply = [this](std::string_view bytes) {
            if (link_ && state_ == State::Live) {
                link_->sendInput(bytes);
            }
        };
        events.title = [this](std::string_view title) {
            title_ = std::string(title);
            changed();
        };
        events.palette = &palette_;
        events.setPaletteColor = [this](std::size_t index, Rgb8 color) {
            if (index < palette_.colors.size()) {
                palette_.colors[index] = color;
            }
        };
        events.resetPaletteColor = [this](std::optional<std::size_t> index) {
            if (!index) {
                palette_.colors = defaultPalette_.colors;
            }
            else if (*index < palette_.colors.size()) {
                palette_.colors[*index] = defaultPalette_.colors[*index];
            }
        };
        parser_ = std::make_unique<VtParser>(*model_, std::move(events));
    }
    void notice(const std::string &text) {
        parser_->feed("\r\n[" + text + "]\r\n");
        changed();
    }
    void onExited(int code, int signal) {
        if (ended()) {
            return;
        }
        state_ = State::Exited;
        exitCode_ = code;
        exitSignal_ = signal;
        notice("process exited: " + (signal ? "signal " + std::to_string(signal)
                                            : std::to_string(code)));
    }
    void onClosed(std::string_view reason) {
        if (ended()) {
            return; // The first reason stands.
        }
        const auto code = errorCode_ ? errorCode_() : std::nullopt;
        if (code == protocol::ErrorCode::Replaced) {
            state_ = State::Replaced;
            notice(config_.replacedNotice);
        }
        else if (code == protocol::ErrorCode::ResumeUnsupported) {
            state_ = State::Unavailable;
            notice("session unavailable");
        }
        else {
            state_ = State::Disconnected;
            notice("disconnected: " + std::string(reason));
        }
    }
    void changed() {
        if (changed_) {
            changed_();
        }
    }

    Config config_;
    const Palette defaultPalette_;
    Palette palette_;
    CursorGlide glide_;
    std::unique_ptr<TerminalModel> model_;
    std::unique_ptr<VtParser> parser_;
    TerminalLink *link_ = nullptr;
    ErrorSource errorCode_;
    State state_ = State::Live;
    int exitCode_ = 0;
    int exitSignal_ = 0;
    std::string title_;
    std::function<void()> changed_;
};
} // namespace frontend
