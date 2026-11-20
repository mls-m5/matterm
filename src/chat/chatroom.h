#pragma once

#include "chatproto.h"
#include "chattext.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The whole server side of the chat as a state machine: connections, names,
// validation, rate limits, caps, live drafts, the ring of messages, bans and
// the operator's commands. It does no I/O and reads no clock: the adapter
// (chatd) feeds it events with the current time and executes the actions it
// returns, in order. See docs/CHAT.md.
namespace chat {

using Millis = std::int64_t; // From an arbitrary start; only differences count.
using ConnId = std::uint32_t;

struct RoomConfig {
    std::size_t ringSlots = 200;
    Millis ttl = 45 * 60 * 1000; // Messages older than this leave the ring.
    std::size_t maxConnections = 64;
    std::size_t maxPerSource = 3;
    std::size_t maxUnhelloed = 16; // Connections that have not said hello yet.
    Millis helloDeadline = 5000;
    Millis lockBackstop = 6500; // The server locks a still bubble after this.
    // Messages: one token per `messageRefill`, at most `messageBurst` saved.
    Millis messageRefill = 2000;
    int messageBurst = 3;
    std::size_t messagesPerMinute = 120; // The whole room.
    // Drafts: forwarded at most this often per connection; more than
    // `draftFloodPerSecond` lines in a second is a violation.
    Millis draftForward = 333;
    int draftFloodPerSecond = 10;
    std::size_t maxTypersShown = 6;
    Millis onlineInterval = 1000;
    Millis nickInterval = 10000;
    std::size_t maxBacklogBytes = 16 * 1024; // A slower reader is dropped.
    int violationsToKick = 5;                // Within violationWindow.
    Millis violationWindow = 60 * 1000;
    Millis kickBan = 5 * 60 * 1000;
};

struct Action {
    ConnId conn = 0;
    std::string line; // Sent as one text frame when `close` is false.
    bool close = false;
};
using Actions = std::vector<Action>;

class ChatRoom {
public:
    explicit ChatRoom(RoomConfig config = {});

    // A new connection from `source` (a key such as sourceKey()).
    Actions connected(ConnId id, const std::string &source, Millis now);
    // A text frame from a connection.
    Actions line(ConnId id, std::string_view frame, Millis now);
    // A frame the room must not accept (binary, fragmented, too large).
    Actions badFrame(ConnId id, Millis now);
    // How many bytes are waiting to be written to the connection.
    Actions backlog(ConnId id, std::size_t bytes, Millis now);
    Actions closed(ConnId id, Millis now);
    Actions tick(Millis now);
    // An operator command (docs/CHAT.md, section 7); `reply` is for the
    // console.
    Actions admin(std::string_view command, Millis now, std::string &reply);
    // When tick() next has something to do; empty when nothing is pending.
    std::optional<Millis> nextDeadline() const;

    struct Stats {
        std::size_t connections = 0, online = 0, ring = 0, bans = 0;
        std::uint64_t messages = 0, violations = 0;
    };
    Stats stats() const;
    bool locked() const noexcept {
        return locked_;
    }

private:
    struct Entry {
        std::uint64_t serial = 0;
        Millis time = 0;
        std::string name, text;
    };
    struct Conn {
        std::string source;
        bool joined = false;
        std::string name;
        Millis connectedAt = 0, joinedAt = 0;
        // The live bubble.
        std::uint32_t bubble = 0;
        bool bubbleLocked = true; // No open bubble yet.
        std::string draft;
        Millis changedAt = 0;
        bool dirty = false; // The draft changed since it was forwarded.
        Millis forwardedAt = -1000000;
        bool shown = false; // Others currently see this draft.
        // Limits.
        double tokens = 0;
        Millis tokenAt = 0;
        int draftsThisSecond = 0;
        Millis draftSecondStart = 0;
        int violations = 0;
        Millis violationStart = 0;
        Millis lastNick = -1000000;
    };

    void send(Actions &out, ConnId id, const ServerLine &line);
    void sendError(Actions &out, ConnId id, ErrorCode code, const char *text);
    void sendAll(Actions &out, const ServerLine &line, ConnId except = 0);
    void drop(Actions &out, ConnId id, Millis now, bool announce);
    void violation(Actions &out, ConnId id, Millis now, const char *why);
    bool takeToken(Conn &c, Millis now);
    bool lockBubble(
        Actions &out, ConnId id, Conn &c, Millis now, bool explicitLock);
    void clearDraft(Actions &out, ConnId id, Conn &c);
    void join(
        Actions &out, ConnId id, Conn &c, const std::string &name, Millis now);
    void expireRing(Millis now);
    std::size_t joinedCount() const;
    ConnId findByName(const std::string &name) const;

    RoomConfig config_;
    std::map<ConnId, Conn> conns_;
    std::map<std::string, std::size_t> perSource_;
    std::map<std::string, Millis> bans_;  // Source to the time the ban ends.
    std::map<std::string, Millis> mutes_; // Source to the time the mute ends.
    std::vector<Entry> ring_;             // Oldest first.
    std::vector<Millis> recentMessages_;  // For the room's per-minute cap.
    std::uint64_t nextSerial_ = 1;
    std::uint64_t messages_ = 0, violations_ = 0;
    bool locked_ = false;
    std::uint32_t lastOnline_ = 0;
    Millis onlineSentAt_ = -1000000;
    std::uint32_t lastTypers_ = 0;
    Millis now_ = 0;
};

} // namespace chat
