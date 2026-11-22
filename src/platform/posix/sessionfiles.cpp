#include "sessionfiles.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frontend {
namespace {

struct Fd {
    int value = -1;
    ~Fd() {
        if (value >= 0) ::close(value);
    }
};

void fail(std::string *error, const std::string &what) {
    if (error) *error = what + ": " + std::strerror(errno);
}

// Ours, a plain file or folder, and not writable by anyone else.
bool ownedByUs(const struct stat &st, bool directory) {
    return st.st_uid == ::geteuid() && (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) &&
           (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

} // namespace

std::string SessionFiles::defaultDirectory() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] == '/') {
        return std::string(xdg) + "/matterm/sessions";
    }
    if (const char *home = std::getenv("HOME"); home && home[0] == '/') {
        return std::string(home) + "/.config/matterm/sessions";
    }
    return {};
}

bool SessionFiles::ensureDirectory(std::string *error) const {
    if (directory_.empty() || directory_.front() != '/') {
        errno = EINVAL;
        fail(error, "no session folder");
        return false;
    }
    // Create every missing part, owner only.
    for (std::size_t at = 1; at <= directory_.size(); ++at) {
        if (at != directory_.size() && directory_[at] != '/') continue;
        const std::string part = directory_.substr(0, at);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
            fail(error, "mkdir " + part);
            return false;
        }
    }
    struct stat st {};
    if (::lstat(directory_.c_str(), &st) != 0 || !ownedByUs(st, true)) {
        errno = EACCES;
        fail(error, "the session folder is not ours or is writable by others");
        return false;
    }
    return true;
}

std::optional<SessionSnapshot> SessionFiles::load(const std::string &name) const {
    if (!SessionSnapshot::validName(name)) return std::nullopt;
    Fd fd;
    fd.value = ::open(pathOf(name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd.value < 0) return std::nullopt;
    struct stat st {};
    if (::fstat(fd.value, &st) != 0 || !ownedByUs(st, false) || st.st_size < 0 ||
        std::size_t(st.st_size) > SessionSnapshot::kMaxFile) {
        return std::nullopt;
    }
    std::string text(std::size_t(st.st_size), '\0');
    std::size_t got = 0;
    while (got < text.size()) {
        const auto n = ::read(fd.value, text.data() + got, text.size() - got);
        if (n <= 0) break;
        got += std::size_t(n);
    }
    if (got != text.size()) return std::nullopt;
    auto snapshot = SessionSnapshot::parse(text);
    // The name inside must be the name of the file: a copied file cannot pose as another session.
    if (!snapshot || snapshot->name != name) return std::nullopt;
    return snapshot;
}

std::vector<SessionInfo> SessionFiles::list() const {
    std::vector<SessionInfo> out;
    struct stat dir {};
    if (directory_.empty() || ::lstat(directory_.c_str(), &dir) != 0 || !ownedByUs(dir, true)) return out;
    DIR *d = ::opendir(directory_.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (const dirent *e = ::readdir(d)) {
        const std::string file = e->d_name;
        if (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0) {
            const auto name = file.substr(0, file.size() - 5);
            if (SessionSnapshot::validName(name)) names.push_back(name);
        }
        if (names.size() >= 4 * kMaxSessions) break; // A folder full of junk stays cheap.
    }
    ::closedir(d);
    for (const auto &name : names) {
        if (const auto s = load(name)) {
            SessionInfo info;
            info.name = name;
            info.saved = s->saved;
            info.tabs = s->tabs.size();
            for (const auto &t : s->tabs) info.panes += SessionSnapshot::paneCount(*t.root);
            out.push_back(std::move(info));
        }
    }
    std::sort(out.begin(), out.end(), [](const SessionInfo &a, const SessionInfo &b) {
        return a.saved != b.saved ? a.saved > b.saved : a.name < b.name;
    });
    if (out.size() > kMaxSessions) out.resize(kMaxSessions);
    return out;
}

bool SessionFiles::save(const SessionSnapshot &snapshot, std::string *error) const {
    if (!snapshot.valid()) {
        errno = EINVAL;
        fail(error, "not a valid session");
        return false;
    }
    const std::string text = snapshot.serialize();
    if (text.size() > SessionSnapshot::kMaxFile) {
        errno = EFBIG;
        fail(error, "session too large");
        return false;
    }
    if (!ensureDirectory(error)) return false;
    const std::string target = pathOf(snapshot.name);
    const std::string temp = directory_ + "/.tmp-" + std::to_string(::getpid()) + "-" + snapshot.name;
    ::unlink(temp.c_str()); // A leftover of ours from a crash; O_EXCL below refuses anything else.
    Fd fd;
    fd.value = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd.value < 0) {
        fail(error, "create " + temp);
        return false;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const auto n = ::write(fd.value, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fail(error, "write");
            ::unlink(temp.c_str());
            return false;
        }
        done += std::size_t(n);
    }
    if (::fsync(fd.value) != 0 || ::rename(temp.c_str(), target.c_str()) != 0) {
        fail(error, "replace " + target);
        ::unlink(temp.c_str());
        return false;
    }
    Fd dir;
    dir.value = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir.value >= 0) ::fsync(dir.value); // Best effort: the rename itself is done.
    return true;
}

bool SessionFiles::remove(const std::string &name) const {
    return SessionSnapshot::validName(name) && ::unlink(pathOf(name).c_str()) == 0;
}

} // namespace frontend
