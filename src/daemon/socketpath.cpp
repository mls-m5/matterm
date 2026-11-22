#include "socketpath.h"

#include <cstdlib>
#include <unistd.h>

namespace mtd {
std::string defaultSocketPath() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime == '/') return std::string(runtime) + "/matterm/daemon.sock";
    return "/tmp/matterm-" + std::to_string(geteuid()) + "/daemon.sock";
}
} // namespace mtd
