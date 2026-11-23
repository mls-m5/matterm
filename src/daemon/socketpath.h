#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mtd {
// $XDG_RUNTIME_DIR/matterm/daemon.sock, else /tmp/matterm-<uid>/daemon.sock:
// the name the daemons before the version scheme use, and the one an explicit
// --daemon-socket replaces.
std::string defaultSocketPath();
// The folder of the default socket.
std::string socketDirectory();

// A daemon is identified by its socket, and a new daemon's socket name carries
// its version (docs/tasks/daemon-versioning.md):
// "daemon-<format>.<breaking>.<additive>.sock" in the folder of the default
// socket. Several daemons of different versions can run side by side.
std::string versionedSocketName(std::uint16_t format,
                                std::uint16_t breaking,
                                std::uint16_t additive);
// The default socket of a daemon of this version.
std::string versionedSocketPath(std::uint16_t format,
                                std::uint16_t breaking,
                                std::uint16_t additive);
// The socket for that version in the folder of `socketPath` (a custom path gets
// the version before its ".sock").
std::string versionedSiblingSocket(const std::string &socketPath,
                                   std::uint16_t format,
                                   std::uint16_t breaking,
                                   std::uint16_t additive);

struct SocketVersion {
    std::uint16_t format = 1, breaking = 3, additive = 0;
};
// "daemon-1.3.1.sock" -> 1.3.1; "daemon.sock" -> 1.3.0 (the daemons before the
// scheme); anything else (also ".lock" files) is not a daemon socket.
std::optional<SocketVersion> parseSocketName(const std::string &fileName);
// The daemon sockets of a folder (full paths), the newest version first (by the
// numbers).
std::vector<std::string> daemonSocketsIn(const std::string &directory);
// The socket a client without --daemon-socket should try: the newest in the
// default folder, else the default socket (which then does not exist).
std::string newestDaemonSocket();
} // namespace mtd
