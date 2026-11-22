#pragma once

#include "paneprobe.h"

// Linux /proc reader for probePane(): the folder and the command line of a process.
// Read-only; a process that vanished or is not ours just yields nothing.
namespace frontend {
class LinuxProcSource final : public ProcSource {
public:
    std::optional<std::string> cwd(int pid) const override;
    std::optional<std::vector<std::string>> cmdline(int pid) const override;
};
} // namespace frontend
