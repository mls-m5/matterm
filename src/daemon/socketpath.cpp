#include "socketpath.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <tuple>
#include <unistd.h>

namespace mtd {

std::string socketDirectory() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime == '/')
        return std::string(runtime) + "/matterm";
    return "/tmp/matterm-" + std::to_string(geteuid());
}

std::string defaultSocketPath() {
    return socketDirectory() + "/daemon.sock";
}

std::string versionedSocketName(std::uint16_t format,
                                std::uint16_t breaking,
                                std::uint16_t additive) {
    return "daemon-" + std::to_string(format) + "." + std::to_string(breaking) +
           "." + std::to_string(additive) + ".sock";
}

std::string versionedSocketPath(std::uint16_t format,
                                std::uint16_t breaking,
                                std::uint16_t additive) {
    return socketDirectory() + "/" +
           versionedSocketName(format, breaking, additive);
}

std::string versionedSiblingSocket(const std::string &socketPath,
                                   std::uint16_t format,
                                   std::uint16_t breaking,
                                   std::uint16_t additive) {
    const auto slash = socketPath.rfind('/');
    const std::string dir = slash == std::string::npos
                                ? std::string()
                                : socketPath.substr(0, slash + 1);
    std::string name =
        slash == std::string::npos ? socketPath : socketPath.substr(slash + 1);
    if (parseSocketName(name))
        return dir + versionedSocketName(format, breaking, additive);
    const std::string extension = ".sock";
    if (name.size() > extension.size() &&
        name.compare(
            name.size() - extension.size(), extension.size(), extension) == 0) {
        name.resize(name.size() - extension.size());
    }
    return dir + name + "-" + std::to_string(format) + "." +
           std::to_string(breaking) + "." + std::to_string(additive) + ".sock";
}

std::optional<SocketVersion> parseSocketName(const std::string &fileName) {
    if (fileName == "daemon.sock")
        return SocketVersion{};
    const std::string prefix = "daemon-", suffix = ".sock";
    if (fileName.size() <= prefix.size() + suffix.size() ||
        fileName.compare(0, prefix.size(), prefix) != 0 ||
        fileName.compare(
            fileName.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const std::string numbers = fileName.substr(
        prefix.size(), fileName.size() - prefix.size() - suffix.size());
    unsigned long part[3] = {0, 0, 0};
    int index = 0;
    bool digit = false;
    for (const char c : numbers) {
        if (c == '.') {
            if (!digit || ++index > 2)
                return std::nullopt;
            digit = false;
        }
        else if (c >= '0' && c <= '9') {
            part[index] = part[index] * 10 + unsigned(c - '0');
            if (part[index] > 65535)
                return std::nullopt;
            digit = true;
        }
        else {
            return std::nullopt;
        }
    }
    if (index != 2 || !digit)
        return std::nullopt;
    return SocketVersion{
        std::uint16_t(part[0]), std::uint16_t(part[1]), std::uint16_t(part[2])};
}

std::vector<std::string> daemonSocketsIn(const std::string &directory) {
    std::vector<std::pair<SocketVersion, std::string>> found;
    DIR *handle = ::opendir(directory.c_str());
    if (!handle)
        return {};
    while (const dirent *entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (const auto v = parseSocketName(name))
            found.emplace_back(*v, directory + "/" + name);
    }
    ::closedir(handle);
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        const auto key = [](const SocketVersion &v) {
            return std::tie(v.format, v.breaking, v.additive);
        };
        return key(a.first) != key(b.first) ? key(a.first) > key(b.first)
                                            : a.second < b.second;
    });
    std::vector<std::string> out;
    for (auto &entry : found)
        out.push_back(std::move(entry.second));
    return out;
}

std::string newestDaemonSocket() {
    const auto found = daemonSocketsIn(socketDirectory());
    return found.empty() ? defaultSocketPath() : found.front();
}

} // namespace mtd
