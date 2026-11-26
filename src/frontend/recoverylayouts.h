#pragma once
#include "sessionfiles.h"
#include "sha256.h"
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>

namespace frontend {
// Atomic layout files, scoped to the daemon endpoint. A lifetime lock hides
// checkpoints belonging to live windows; SIGKILL releases it automatically.
class RecoveryLayouts {
public:
    RecoveryLayouts(const std::string &base, const std::string &endpoint)
        : files_(scope(base, endpoint)) {
        if (base.empty())
            return;
        std::error_code ec;
        const bool made =
            std::filesystem::create_directories(files_.directory(), ec);
        if (ec)
            return;
        if (made) {
            std::filesystem::permissions(
                files_.directory(), std::filesystem::perms::owner_all, ec);
            if (ec)
                return;
        }
        std::string pattern = files_.directory() + "/windowXXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back(0);
        fd_ = ::mkstemp(path.data());
        if (fd_ < 0)
            return;
        ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        token_ = std::filesystem::path(path.data()).filename().string();
    }
    ~RecoveryLayouts() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    RecoveryLayouts(const RecoveryLayouts &) = delete;
    const SessionFiles &files() const {
        return files_;
    }
    bool available(const std::string &name) const {
        const auto dash = name.find('-');
        if (dash == std::string::npos || !SessionSnapshot::validName(name))
            return false;
        const std::string path =
            files_.directory() + "/" + name.substr(0, dash);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return false;
        const bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
        ::close(fd);
        return free;
    }
    bool checkpoint(std::uint64_t tab,
                    SavedTab saved,
                    std::set<std::string> &current,
                    std::string &error) {
        if (fd_ < 0) {
            error = "cannot lock recovery directory";
            return false;
        }
        SessionSnapshot s;
        s.name = token_ + "-" + std::to_string(tab);
        current.insert(s.name);
        s.tabs.push_back(std::move(saved));
        const std::string content =
            s.serialize(); // saved=0: compare structure, not the clock.
        if (last_[s.name] == content)
            return true;
        s.saved = std::time(nullptr);
        if (!files_.save(s, &error))
            return false;
        last_[s.name] = content;
        return true;
    }
    void retireExcept(const std::set<std::string> &current) {
        for (auto it = last_.begin(); it != last_.end();) {
            if (!current.count(it->first) && files_.remove(it->first))
                it = last_.erase(it);
            else
                ++it;
        }
    }
    void retire() {
        retireExcept({});
    }

private:
    static std::string scope(const std::string &base,
                             const std::string &endpoint) {
        if (base.empty())
            return {};
        Sha256 hash;
        hash.update(endpoint.data(), endpoint.size());
        return base + "/recovery-" + hash.hex().substr(0, 24);
    }
    SessionFiles files_;
    std::string token_;
    int fd_ = -1;
    std::map<std::string, std::string> last_;
};
inline bool recoverableNode(const SavedNode &node) {
    return node.leaf
               ? LayoutSnapshot::validId(node.pane.terminal)
               : recoverableNode(*node.first) && recoverableNode(*node.second);
}
} // namespace frontend
