#pragma once

#include "chatproto.h"
#include "chatroom.h" // Millis

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The client side of the chat as a state machine: no sockets, no clock, no
// terminal. The host opens the connection and tells the client what happened
// (opened, receive, closed, tick); the client says which lines to send. It owns
// the person's own bubble (the draft that is sent while typing, the idle lock,
// `not sent`) and a model of the room for a screen to draw. Everything received
// is parsed and sanitized by chatproto before it gets here. See docs/CHAT.md.
namespace chat {

class ChatClient {
public:
    enum class State { Off, Connecting, Hello, NeedName, Joined, Closed };

    struct Config {
        std::string name;             // The name to say hello with.
        Millis idleLock = 5000;       // Stillness that locks the bubble.
        Millis draftInterval = 334;   // At most about 3 drafts a second.
        std::size_t maxHistory = 500; // Locked bubbles kept for the screen.
        std::size_t maxUsers = 256;
        std::size_t maxEvents = 1000; // Unread events kept.
        int maxBadLines = 20;         // A server that sends more is dropped.
        // Other people's live text arrives in snapshots; it is shown one
        // character at a time, as if it was being typed here (docs/CHAT.md).
        Millis minCharMs = 25; // Floor and ceiling per character...
        Millis maxCharMs = 120;
        Millis maxLagMs = 1500;  // ...but never further behind than this.
        Millis finalCharMs = 10; // Catching up when the text is posted.
        int jitterPercent = 20;  // A little unevenness, seeded.
        std::uint32_t jitterSeed = 1;
    };

    // What to do with the connection after a call.
    struct Out {
        std::vector<std::string> send;
        bool close = false;
    };

    struct Bubble {
        std::uint64_t serial = 0;
        std::string name;
        std::string text;
        Millis at = 0;
        bool own = false;
    };
    struct Typer {
        std::string name;
        std::string text;   // What is shown now (it follows `target`).
        std::string target; // The latest snapshot from the server.
        Millis nextAt = 0;  // When the next character appears or goes.
        Millis interval = 60;
        Millis updatedAt = -1;
        bool finalizing = false; // Posted: finished quickly, then a message.
    };
    enum class OwnState {
        Idle,      // Nothing typed.
        DraftSent, // Typed and being sent live.
        NotSent,   // The connection is gone (or never was); the text stays.
    };

    // Things for a screen to show once, in order.
    struct Event {
        enum class Kind {
            Message,  // A locked bubble (bubble).
            Join,     // name
            Leave,    // name
            Notice,   // text (a moderator or the server)
            Error,    // code/text from the server
            Hide,     // A message was removed (serial).
            State,    // The connection state changed.
            NameTaken // Hello needs another name.
        };
        Kind kind = Kind::Notice;
        Bubble bubble;
        std::string name, text;
        std::uint64_t serial = 0;
        ErrorCode code = ErrorCode::BadLine;
    };

    explicit ChatClient(Config config);

    // The person wants to connect; the host then opens the channel and calls
    // opened(). (Nothing is contacted by the client itself.)
    Out connect(Millis now);
    Out opened(Millis now);
    Out receive(std::string_view frame, Millis now);
    Out closed(Millis now);
    Out tick(Millis now);
    std::optional<Millis> nextDeadline() const;
    // The person leaves.
    Out leave(Millis now);

    // Another name after `name-taken`.
    Out hello(const std::string &name, Millis now);
    Out rename(const std::string &name, Millis now);

    // The whole text of the own bubble after an edit. false: not allowed (a
    // character the room refuses, or too long); nothing changed.
    bool edit(const std::string &text, Millis now, Out &out);
    // Enter: lock the bubble now instead of waiting.
    Out submit(Millis now);

    State state() const noexcept {
        return state_;
    }
    const std::string &name() const noexcept {
        return name_;
    }
    const std::string &draft() const noexcept {
        return draft_;
    }
    OwnState ownState() const noexcept;
    std::uint32_t ownBubble() const noexcept {
        return bubble_;
    }
    const std::deque<Bubble> &history() const noexcept {
        return history_;
    }
    const std::vector<Typer> &typers() const noexcept {
        return typers_;
    }
    std::uint32_t hiddenTypers() const noexcept {
        return hidden_;
    }
    std::uint32_t online() const noexcept {
        return online_;
    }
    const std::vector<std::string> &users() const noexcept {
        return users_;
    }
    std::vector<Event> takeEvents();
    // True once after the shown text of the other people changed (a character
    // appeared or went): a screen that redraws on demand asks this.
    bool takeTypersChanged() {
        const bool changed = typersChanged_;
        typersChanged_ = false;
        return changed;
    }
    const std::string &closeReason() const noexcept {
        return reason_;
    }

private:
    void setState(State s);
    void push(Event e);
    void flushDraft(Out &out, Millis now, bool force);
    void startNextBubble();
    void addHistory(Bubble b);
    void emitMessage(Bubble b);
    void releaseDelayed(Millis now, bool all);
    bool stepTypers(Millis now);
    Millis jittered(Millis interval);

    Config config_;
    State state_ = State::Off;
    std::string name_;
    std::string draft_;     // The own bubble's text.
    std::string sentDraft_; // What the server last got.
    bool sentAny_ = false;
    std::uint32_t bubble_ = 1;
    Millis editedAt_ = 0, draftSentAt_ = -1000000;
    bool lockSent_ = false;
    bool offline_ = false; // Text typed while not joined, or after a drop.
    std::deque<Bubble> history_;
    std::vector<Typer> typers_;
    struct Delayed {
        Bubble bubble;
        Millis releaseAt = 0;
    };
    std::deque<Delayed> delayed_; // Messages waiting for their live text.
    std::uint32_t jitterState_ = 1;
    bool typersChanged_ = false;
    std::uint32_t hidden_ = 0, online_ = 0;
    std::vector<std::string> users_, pendingUsers_;
    std::vector<Event> events_;
    std::string reason_;
    int badLines_ = 0;
};

} // namespace chat
