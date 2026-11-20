#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fakeos {

// The pretend file system: a tree in memory with owners, modes and hard limits
// (a runaway `yes > f` must hit a limit, not memory). Paths are absolute and
// normalized; see normalize() for turning user input into one.
class VirtualFs {
public:
    struct Limits {
        std::size_t maxNodes = 2000;
        std::size_t maxTotalBytes = 1u << 20;
        std::size_t maxFileBytes = 256u << 10;
        std::size_t maxDepth = 32;
        std::size_t maxNameBytes = 255;
    };
    enum class Error {
        None,
        NotFound,
        Exists,
        NotDirectory,
        IsDirectory,
        NotEmpty,
        Permission,
        TooBig,  // File size limit.
        NoSpace, // Node or byte limit of the whole system.
        Invalid, // Bad name or path too deep.
    };
    struct Node {
        bool directory = false;
        std::string data;
        std::map<std::string, std::unique_ptr<Node>> children;
        std::string owner = "root";
        std::uint16_t mode =
            0644;               // rwxrwxrwx; no groups, owner and others only.
        std::int64_t mtime = 0; // Unix seconds.
    };

    VirtualFs();
    explicit VirtualFs(Limits limits);

    // Absolute path for `path` relative to `cwd`: handles ., .., repeated
    // slashes and a trailing slash; ".." at the root stays at the root.
    static std::string normalize(const std::string &cwd,
                                 const std::string &path);
    static std::string parentOf(const std::string &absolute);
    static std::string nameOf(const std::string &absolute);
    static std::string join(const std::string &directory,
                            const std::string &name);
    static const char *message(Error error);

    const Node *find(const std::string &absolute) const;
    std::vector<std::pair<std::string, const Node *>> list(
        const std::string &directory) const;
    bool canRead(const Node &node, const std::string &user) const;
    bool canWrite(const Node &node, const std::string &user) const;
    bool canSearch(const Node &node, const std::string &user) const;

    // Operations as `user` (root bypasses permissions). `now` is Unix seconds.
    Error mkdir(const std::string &absolute,
                const std::string &user,
                std::int64_t now,
                bool parents = false);
    Error write(const std::string &absolute,
                const std::string &data,
                const std::string &user,
                std::int64_t now,
                bool append = false);
    Error touch(const std::string &absolute,
                const std::string &user,
                std::int64_t now);
    Error remove(const std::string &absolute,
                 const std::string &user,
                 bool recursive);
    Error copy(const std::string &from,
               const std::string &to,
               const std::string &user,
               std::int64_t now,
               bool recursive);
    Error move(const std::string &from,
               const std::string &to,
               const std::string &user,
               std::int64_t now);
    Error chmod(const std::string &absolute,
                const std::string &user,
                std::uint16_t mode);

    // Builds the initial world: creates parents, ignores permissions and
    // limits.
    void put(const std::string &absolute,
             const std::string &data,
             const std::string &owner,
             std::uint16_t mode,
             std::int64_t mtime);
    // An existing directory keeps its owner and mode unless `restyle`.
    void putDirectory(const std::string &absolute,
                      const std::string &owner,
                      std::uint16_t mode,
                      std::int64_t mtime,
                      bool restyle = true);

    std::size_t nodeCount() const {
        return nodes_;
    }
    std::size_t totalBytes() const {
        return bytes_;
    }

private:
    Node *findMutable(const std::string &absolute);
    Error checkName(const std::string &absolute) const;
    // Creates one entry in an existing directory the user may write to.
    Error create(const std::string &absolute,
                 bool directory,
                 const std::string &user,
                 std::int64_t now,
                 Node *&created);
    static std::size_t count(const Node &node);
    static std::size_t bytesOf(const Node &node);
    std::unique_ptr<Node> clone(const Node &node,
                                const std::string &owner,
                                std::int64_t now) const;

    Limits limits_;
    std::unique_ptr<Node> root_;
    std::size_t nodes_ = 1;
    std::size_t bytes_ = 0;
};

} // namespace fakeos
