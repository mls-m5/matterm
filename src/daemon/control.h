#pragma once

// Control messages of the local daemon (docs/PROTOCOL.md "Local daemon"): a second family of
// message types next to the terminal protocol, used on a connection that starts with
// ControlHello instead of Hello. Client to daemon types are 0x40..0x4f, daemon to client 0xc0..0xcf,
// so a terminal-protocol decoder sees them as unknown types and an old peer never mistakes them.
// Pure codec: no I/O.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mtd {

// 2: CreateSession and GetSessionInfo (a version 1 daemon answers them with BadMessage).
// 3: SetName, and terminal names in the session list and the session info. The names come in
//    two new reply types that the daemon sends only to a client whose hello said version 3 or
//    more; older clients keep getting the old types, which decode to the same structs without
//    a name. An older daemon answers SetName with BadMessage.
constexpr std::uint16_t kControlVersion = 3;
// The same number under its new name (docs/tasks/daemon-versioning.md,
// version.h): the "breaking" part of format.breaking.additive. `additive`
// counts compatible extensions since breaking 3 began: 1 = the version scheme
// itself (the version in ControlWelcome::build).
constexpr std::uint16_t kControlFormat = 1;
constexpr std::uint16_t kControlBreaking = kControlVersion;
constexpr std::uint16_t kControlAdditive = 2; // 2: GetActivity/Activity.
constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxListed = 256;
constexpr std::size_t kMaxPath = 4096;
constexpr std::size_t kMaxArguments = 32;
constexpr std::size_t kMaxArgument = 1024;

struct ControlHello { std::uint16_t version = kControlVersion; };
// First frame of a connection from the web server (`matterm-server --expose-local-sessions`).
// Such a connection sees and attaches to shared sessions only; it cannot create, end, mark or
// probe sessions, and it is cut when a session it uses stops being shared. Answered with
// ControlWelcome. Then it may send ListSessions (shared ones only) or a terminal Hello.
struct ExposedHello { std::uint16_t version = kControlVersion; };
struct ListSessions {};
struct SetShared { std::string id; bool shared = false; };
struct GetStatus {};
// How long ago a terminal client was last attached (for "stray daemon" lists).
// Additive in 1.3.2; an older daemon answers BadMessage.
struct GetActivity {};
// The owner confirmed ending `expectedSessions` sessions. Runs only when that is still the
// number of live sessions; otherwise the daemon answers with the real count and does nothing.
struct Shutdown { std::uint32_t expectedSessions = 0; };

// Starts a session in `cwd` (empty: the daemon's folder; a folder that does not exist is
// ignored) with an initial size. The command and environment are never the client's choice:
// the daemon runs the user's shell. Answered with SessionCreated; attach with Hello{id}.
struct CreateSession {
    std::string cwd;
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;
};
// What a session runs, for saving and for the close question (read from /proc by the daemon).
struct GetSessionInfo { std::string id; };
// Names a terminal so that it can be told apart in the lists (a laptop window, a browser). At
// most 64 bytes of UTF-8 without control characters; empty removes the name.
struct SetName { std::string id; std::string name; };
// True for a name the daemon stores.
bool validTerminalName(std::string_view name);

struct ControlWelcome {
    std::uint16_t version = kControlVersion;
    std::uint32_t pid = 0;
    std::uint32_t features = 0;
    std::string build; // Free text, for "Show daemon status".
};
struct SessionEntry {
    std::string id;
    bool attached = false;
    bool shared = false;
    std::uint64_t outputEnd = 0;
    std::string name; // Empty: none (or the peer is older than version 3).
};
struct SessionList { std::vector<SessionEntry> sessions; };
struct Status {
    std::uint32_t pid = 0;
    std::uint32_t sessions = 0;
    std::uint32_t attached = 0;
    std::uint32_t shared = 0;
    std::uint32_t clients = 0;
    std::uint64_t uptimeMs = 0;
    std::uint32_t idleExitSeconds = 0;
};
struct SessionCreated { std::string id; };
struct Activity {
    bool attachedNow = false; // A terminal client is attached at this moment.
    std::uint32_t secondsSinceClient =
        0; // Since the last one left (or since the daemon started).
};
struct SessionInfo {
    std::string id;
    std::string cwd;                  // Absolute, empty when unknown.
    bool busy = false;                // The foreground process is not a shell.
    std::vector<std::string> command; // Its command line when busy and safe to keep.
    std::string name;                 // See SetName.
};
struct ShutdownReply {
    bool done = false;           // false: refused because the count was not current
    std::uint32_t sessions = 0;  // The live sessions at that moment
};
enum class ControlErrorCode : std::uint16_t {
    BadMessage = 1,
    NotControl = 2,
    UnknownSession = 3,
    Busy = 4,
    StartFailed = 5 // CreateSession: registry full or the shell did not start.
};
struct ControlError {
    ControlErrorCode code = ControlErrorCode::BadMessage;
    std::string reason;
};

using ControlRequest = std::variant<ControlHello,
                                    ExposedHello,
                                    ListSessions,
                                    SetShared,
                                    GetStatus,
                                    Shutdown,
                                    CreateSession,
                                    GetSessionInfo,
                                    SetName,
                                    GetActivity>;
using ControlReply = std::variant<ControlWelcome,
                                  SessionList,
                                  Status,
                                  ShutdownReply,
                                  ControlError,
                                  SessionCreated,
                                  SessionInfo,
                                  Activity>;

// True for a first byte in the control range (either direction).
inline bool isControlType(std::uint8_t type) { return (type & 0xf0) == 0x40 || (type & 0xf0) == 0xc0; }

bool encode(const ControlRequest &message, std::string &out);
// `peerVersion`: the version the other side said; below 3 the names are left out (old types).
bool encode(const ControlReply &message, std::string &out, std::uint16_t peerVersion = kControlVersion);
// Exactly one complete message; nullopt for anything malformed (truncated, trailing bytes, bad
// UTF-8, out-of-range value, wrong direction).
std::optional<ControlRequest> decodeRequest(std::string_view bytes);
std::optional<ControlReply> decodeReply(std::string_view bytes);

} // namespace mtd
