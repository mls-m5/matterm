#pragma once

// Where settings.json is and how it is read and written: a small file system
// interface (so that the watcher is tested without files) and the real one.
// Writing is atomic: a temporary file next to it, synced, private (0600),
// renamed over; a reader never sees half a file. The folders are made private
// (0700).
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace frontend::settings {

// $XDG_CONFIG_HOME/matterm/settings.json (when absolute), else
// $HOME/.config/matterm/settings.json; empty when neither is known.
inline std::string settingsPath(const char *xdgConfigHome, const char *home) {
    if (xdgConfigHome && xdgConfigHome[0] == '/') {
        return std::string(xdgConfigHome) + "/matterm/settings.json";
    }
    if (home && home[0] == '/') {
        return std::string(home) + "/.config/matterm/settings.json";
    }
    return {};
}

struct FileStat {
    std::int64_t mtime = 0; // Nanoseconds.
    std::uint64_t size = 0;
    friend bool operator==(const FileStat &a, const FileStat &b) {
        return a.mtime == b.mtime && a.size == b.size;
    }
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual std::optional<FileStat> stat(const std::string &path) = 0;
    virtual bool read(const std::string &path, std::string &out) = 0;
    virtual bool writeAtomic(const std::string &path,
                             const std::string &text,
                             std::string &error) = 0;
};

class RealFileSystem final : public FileSystem {
public:
    std::optional<FileStat> stat(const std::string &path) override {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return std::nullopt;
        }
        return FileStat{std::int64_t(st.st_mtim.tv_sec) * 1000000000 +
                            st.st_mtim.tv_nsec,
                        std::uint64_t(st.st_size)};
    }
    bool read(const std::string &path, std::string &out) override {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        out.clear();
        char buffer[8192];
        for (;;) {
            const auto n = ::read(fd, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            out.append(buffer, std::size_t(n));
            if (out.size() > (2u << 20)) { // Far more than a settings file; the
                                           // parser refuses it.
                break;
            }
        }
        ::close(fd);
        return true;
    }
    bool writeAtomic(const std::string &path,
                     const std::string &text,
                     std::string &error) override {
        const auto slash = path.rfind('/');
        if (slash == std::string::npos || slash == 0) {
            error = "no folder in " + path;
            return false;
        }
        const std::string dir = path.substr(0, slash);
        if (!makeFolders(dir, error)) {
            return false;
        }
        std::string temporary = dir + "/.settings.json.XXXXXX";
        const int fd = ::mkstemp(temporary.data());
        if (fd < 0) {
            error = "cannot write in " + dir + ": " + std::strerror(errno);
            return false;
        }
        bool ok = ::fchmod(fd, 0600) == 0;
        for (std::size_t done = 0; ok && done < text.size();) {
            const auto n = ::write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            ok = n > 0;
            if (ok) {
                done += std::size_t(n);
            }
        }
        ok = ok && ::fsync(fd) == 0;
        const int saved = errno;
        ::close(fd);
        if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
            error = std::string("cannot write ") + path + ": " +
                    std::strerror(ok ? errno : saved);
            ::unlink(temporary.c_str());
            return false;
        }
        return true;
    }

private:
    static bool makeFolders(const std::string &dir, std::string &error) {
        struct stat st {};
        if (::stat(dir.c_str(), &st) == 0) {
            return S_ISDIR(st.st_mode) ||
                   (error = dir + " is not a folder", false);
        }
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos && slash > 0 &&
            !makeFolders(dir.substr(0, slash), error)) {
            return false;
        }
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot make " + dir + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }
};

} // namespace frontend::settings
