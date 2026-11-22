#pragma once
#include <string>

namespace mtd {
// $XDG_RUNTIME_DIR/matterm/daemon.sock, else /tmp/matterm-<uid>/daemon.sock.
std::string defaultSocketPath();
} // namespace mtd
