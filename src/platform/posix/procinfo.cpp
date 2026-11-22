#include "procinfo.h"

#include <climits>
#include <fcntl.h>
#include <unistd.h>

namespace frontend {

std::optional<std::string> LinuxProcSource::cwd(int pid) const {
    if (pid <= 0) return std::nullopt;
    const std::string link = "/proc/" + std::to_string(pid) + "/cwd";
    std::string buffer(PATH_MAX, '\0');
    const auto n = ::readlink(link.c_str(), buffer.data(), buffer.size());
    if (n <= 0 || std::size_t(n) >= buffer.size()) return std::nullopt;
    buffer.resize(std::size_t(n));
    return buffer; // A deleted folder reads "... (deleted)": the caller's plainPath/exists check decides.
}

std::optional<std::vector<std::string>> LinuxProcSource::cmdline(int pid) const {
    if (pid <= 0) return std::nullopt;
    const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    // Bounded: one more byte than is ever kept tells us the line was too long.
    std::string data;
    char chunk[1024];
    while (data.size() <= PaneProbeLimits::kMaxCommand + 1) {
        const auto n = ::read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        data.append(chunk, std::size_t(n));
    }
    ::close(fd);
    if (data.empty()) return std::nullopt; // A kernel thread or a zombie.
    std::vector<std::string> argv;
    std::size_t start = 0;
    while (start < data.size()) {
        auto end = data.find('\0', start);
        if (end == std::string::npos) end = data.size();
        argv.emplace_back(data, start, end - start);
        start = end + 1;
        if (argv.size() > PaneProbeLimits::kMaxArguments) break; // Too many: plainCommand refuses it.
    }
    return argv;
}

} // namespace frontend
