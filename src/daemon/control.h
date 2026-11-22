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

constexpr std::uint16_t kControlVersion = 1;
constexpr std::size_t kMaxListed = 256;

struct ControlHello { std::uint16_t version = kControlVersion; };
struct ListSessions {};
struct SetShared { std::string id; bool shared = false; };
struct GetStatus {};
// The owner confirmed ending `expectedSessions` sessions. Runs only when that is still the
// number of live sessions; otherwise the daemon answers with the real count and does nothing.
struct Shutdown { std::uint32_t expectedSessions = 0; };

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
struct ShutdownReply {
    bool done = false;           // false: refused because the count was not current
    std::uint32_t sessions = 0;  // The live sessions at that moment
};
enum class ControlErrorCode : std::uint16_t { BadMessage = 1, NotControl = 2, UnknownSession = 3, Busy = 4 };
struct ControlError {
    ControlErrorCode code = ControlErrorCode::BadMessage;
    std::string reason;
};

using ControlRequest = std::variant<ControlHello, ListSessions, SetShared, GetStatus, Shutdown>;
using ControlReply = std::variant<ControlWelcome, SessionList, Status, ShutdownReply, ControlError>;

// True for a first byte in the control range (either direction).
inline bool isControlType(std::uint8_t type) { return (type & 0xf0) == 0x40 || (type & 0xf0) == 0xc0; }

bool encode(const ControlRequest &message, std::string &out);
bool encode(const ControlReply &message, std::string &out);
// Exactly one complete message; nullopt for anything malformed (truncated, trailing bytes, bad
// UTF-8, out-of-range value, wrong direction).
std::optional<ControlRequest> decodeRequest(std::string_view bytes);
std::optional<ControlReply> decodeReply(std::string_view bytes);

} // namespace mtd
