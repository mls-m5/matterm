#include "virtualfs.h"

#include <algorithm>

namespace fakeos {

VirtualFs::VirtualFs()
    : VirtualFs(Limits{}) {}

VirtualFs::VirtualFs(Limits limits)
    : limits_(limits)
    , root_(std::make_unique<Node>()) {
    root_->directory = true;
    root_->mode = 0755;
}

std::string VirtualFs::normalize(const std::string &cwd,
                                 const std::string &path) {
    std::vector<std::string> parts;
    auto walk = [&](const std::string &text) {
        std::size_t i = 0;
        while (i <= text.size()) {
            const auto end = text.find('/', i);
            const auto part =
                text.substr(i, end == std::string::npos ? end : end - i);
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            }
            else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            if (end == std::string::npos) {
                break;
            }
            i = end + 1;
        }
    };
    if (path.empty() || path[0] != '/') {
        walk(cwd);
    }
    walk(path);
    std::string out;
    for (const auto &part : parts) {
        out += "/" + part;
    }
    return out.empty() ? "/" : out;
}

std::string VirtualFs::parentOf(const std::string &absolute) {
    const auto slash = absolute.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/"
                                                    : absolute.substr(0, slash);
}

std::string VirtualFs::nameOf(const std::string &absolute) {
    return absolute == "/" ? "/" : absolute.substr(absolute.rfind('/') + 1);
}

std::string VirtualFs::join(const std::string &directory,
                            const std::string &name) {
    return directory == "/" ? "/" + name : directory + "/" + name;
}

const char *VirtualFs::message(Error error) {
    switch (error) {
    case Error::None:
        return "";
    case Error::NotFound:
        return "No such file or directory";
    case Error::Exists:
        return "File exists";
    case Error::NotDirectory:
        return "Not a directory";
    case Error::IsDirectory:
        return "Is a directory";
    case Error::NotEmpty:
        return "Directory not empty";
    case Error::Permission:
        return "Permission denied";
    case Error::TooBig:
        return "File too large";
    case Error::NoSpace:
        return "No space left on device";
    case Error::Invalid:
        return "Invalid argument";
    }
    return "Error";
}

const VirtualFs::Node *VirtualFs::find(const std::string &absolute) const {
    const Node *node = root_.get();
    std::size_t i = 1;
    while (node && i < absolute.size()) {
        if (!node->directory) {
            return nullptr;
        }
        const auto end = absolute.find('/', i);
        const auto name =
            absolute.substr(i, end == std::string::npos ? end : end - i);
        const auto it = node->children.find(name);
        node = it == node->children.end() ? nullptr : it->second.get();
        i = end == std::string::npos ? absolute.size() : end + 1;
    }
    return node;
}

VirtualFs::Node *VirtualFs::findMutable(const std::string &absolute) {
    return const_cast<Node *>(find(absolute));
}

std::vector<std::pair<std::string, const VirtualFs::Node *>> VirtualFs::list(
    const std::string &directory) const {
    std::vector<std::pair<std::string, const Node *>> out;
    if (const auto *node = find(directory); node && node->directory) {
        for (const auto &[name, child] : node->children) {
            out.emplace_back(name, child.get());
        }
    }
    return out;
}

bool VirtualFs::canRead(const Node &node, const std::string &user) const {
    return user == "root" ||
           (node.owner == user ? (node.mode & 0400) : (node.mode & 0004)) != 0;
}
bool VirtualFs::canWrite(const Node &node, const std::string &user) const {
    return user == "root" ||
           (node.owner == user ? (node.mode & 0200) : (node.mode & 0002)) != 0;
}
bool VirtualFs::canSearch(const Node &node, const std::string &user) const {
    return user == "root" ||
           (node.owner == user ? (node.mode & 0100) : (node.mode & 0001)) != 0;
}

VirtualFs::Error VirtualFs::checkName(const std::string &absolute) const {
    if (absolute == "/") {
        return Error::Invalid;
    }
    std::size_t depth = 0;
    std::size_t i = 1;
    while (i <= absolute.size()) {
        const auto end = absolute.find('/', i);
        const auto len = (end == std::string::npos ? absolute.size() : end) - i;
        if (len > limits_.maxNameBytes) {
            return Error::Invalid;
        }
        ++depth;
        if (end == std::string::npos) {
            break;
        }
        i = end + 1;
    }
    return depth > limits_.maxDepth ? Error::Invalid : Error::None;
}

std::size_t VirtualFs::count(const Node &node) {
    std::size_t n = 1;
    for (const auto &[name, child] : node.children) {
        n += count(*child);
    }
    return n;
}

std::size_t VirtualFs::bytesOf(const Node &node) {
    std::size_t n = node.data.size();
    for (const auto &[name, child] : node.children) {
        n += bytesOf(*child);
    }
    return n;
}

VirtualFs::Error VirtualFs::create(const std::string &absolute,
                                   bool directory,
                                   const std::string &user,
                                   std::int64_t now,
                                   Node *&created) {
    created = nullptr;
    if (const auto e = checkName(absolute); e != Error::None) {
        return e;
    }
    Node *parent = findMutable(parentOf(absolute));
    if (!parent) {
        return Error::NotFound;
    }
    if (!parent->directory) {
        return Error::NotDirectory;
    }
    if (!canWrite(*parent, user) || !canSearch(*parent, user)) {
        return Error::Permission;
    }
    const auto name = nameOf(absolute);
    if (parent->children.count(name)) {
        return Error::Exists;
    }
    if (nodes_ + 1 > limits_.maxNodes) {
        return Error::NoSpace;
    }
    auto node = std::make_unique<Node>();
    node->directory = directory;
    node->owner = user;
    node->mode = directory ? 0755 : 0644;
    node->mtime = now;
    created = node.get();
    parent->children.emplace(name, std::move(node));
    parent->mtime = now;
    ++nodes_;
    return Error::None;
}

VirtualFs::Error VirtualFs::mkdir(const std::string &absolute,
                                  const std::string &user,
                                  std::int64_t now,
                                  bool parents) {
    if (parents) {
        // Create each missing ancestor first.
        std::string path;
        std::size_t i = 1;
        while (i <= absolute.size()) {
            const auto end = absolute.find('/', i);
            path = absolute.substr(
                0, end == std::string::npos ? absolute.size() : end);
            if (const auto *existing = find(path)) {
                if (!existing->directory) {
                    return Error::NotDirectory;
                }
            }
            else {
                Node *made = nullptr;
                if (const auto e = create(path, true, user, now, made);
                    e != Error::None) {
                    return e;
                }
            }
            if (end == std::string::npos) {
                break;
            }
            i = end + 1;
        }
        return Error::None;
    }
    Node *made = nullptr;
    return create(absolute, true, user, now, made);
}

VirtualFs::Error VirtualFs::write(const std::string &absolute,
                                  const std::string &data,
                                  const std::string &user,
                                  std::int64_t now,
                                  bool append) {
    Node *node = findMutable(absolute);
    if (node && node->directory) {
        return Error::IsDirectory;
    }
    if (node && !canWrite(*node, user)) {
        return Error::Permission;
    }
    const auto oldSize = node ? node->data.size() : 0;
    const auto newSize = append ? oldSize + data.size() : data.size();
    // Limits are checked before anything is created.
    if (newSize > limits_.maxFileBytes) {
        return Error::TooBig;
    }
    if (bytes_ - oldSize + newSize > limits_.maxTotalBytes) {
        return Error::NoSpace;
    }
    if (!node) {
        if (const auto e = create(absolute, false, user, now, node);
            e != Error::None) {
            return e;
        }
    }
    bytes_ = bytes_ - node->data.size() + newSize;
    if (append) {
        node->data += data;
    }
    else {
        node->data = data;
    }
    node->mtime = now;
    return Error::None;
}

VirtualFs::Error VirtualFs::touch(const std::string &absolute,
                                  const std::string &user,
                                  std::int64_t now) {
    if (Node *node = findMutable(absolute)) {
        if (!canWrite(*node, user)) {
            return Error::Permission;
        }
        node->mtime = now;
        return Error::None;
    }
    Node *made = nullptr;
    return create(absolute, false, user, now, made);
}

VirtualFs::Error VirtualFs::remove(const std::string &absolute,
                                   const std::string &user,
                                   bool recursive) {
    if (absolute == "/") {
        return Error::Invalid;
    }
    Node *parent = findMutable(parentOf(absolute));
    if (!parent) {
        return Error::NotFound;
    }
    const auto it = parent->children.find(nameOf(absolute));
    if (it == parent->children.end()) {
        return Error::NotFound;
    }
    if (!canWrite(*parent, user) || !canSearch(*parent, user)) {
        return Error::Permission;
    }
    Node &node = *it->second;
    if (node.directory && !node.children.empty() && !recursive) {
        return Error::NotEmpty;
    }
    nodes_ -= count(node);
    bytes_ -= bytesOf(node);
    parent->children.erase(it);
    return Error::None;
}

std::unique_ptr<VirtualFs::Node> VirtualFs::clone(const Node &node,
                                                  const std::string &owner,
                                                  std::int64_t now) const {
    auto copy = std::make_unique<Node>();
    copy->directory = node.directory;
    copy->data = node.data;
    copy->owner = owner;
    copy->mode = node.mode;
    copy->mtime = now;
    for (const auto &[name, child] : node.children) {
        copy->children.emplace(name, clone(*child, owner, now));
    }
    return copy;
}

VirtualFs::Error VirtualFs::copy(const std::string &from,
                                 const std::string &to,
                                 const std::string &user,
                                 std::int64_t now,
                                 bool recursive) {
    const Node *source = find(from);
    if (!source) {
        return Error::NotFound;
    }
    if (!canRead(*source, user)) {
        return Error::Permission;
    }
    if (source->directory && !recursive) {
        return Error::IsDirectory;
    }
    if (to == from ||
        (to.size() > from.size() && to.compare(0, from.size(), from) == 0 &&
         to[from.size()] == '/')) {
        return Error::Invalid; // Into itself.
    }
    if (find(to)) {
        return Error::Exists;
    }
    const auto addNodes = count(*source);
    const auto addBytes = bytesOf(*source);
    if (nodes_ + addNodes > limits_.maxNodes ||
        bytes_ + addBytes > limits_.maxTotalBytes) {
        return Error::NoSpace;
    }
    Node *made = nullptr;
    if (const auto e = create(to, source->directory, user, now, made);
        e != Error::None) {
        return e;
    }
    // `create` made an empty node; fill it from the source.
    auto replacement = clone(*source, user, now);
    made->data = std::move(replacement->data);
    made->mode = replacement->mode;
    made->children = std::move(replacement->children);
    nodes_ += addNodes - 1;
    bytes_ += addBytes;
    return Error::None;
}

VirtualFs::Error VirtualFs::move(const std::string &from,
                                 const std::string &to,
                                 const std::string &user,
                                 std::int64_t now) {
    if (from == "/" || from == to) {
        return Error::Invalid;
    }
    if (to.size() > from.size() && to.compare(0, from.size(), from) == 0 &&
        to[from.size()] == '/') {
        return Error::Invalid;
    }
    Node *oldParent = findMutable(parentOf(from));
    if (!oldParent) {
        return Error::NotFound;
    }
    const auto it = oldParent->children.find(nameOf(from));
    if (it == oldParent->children.end()) {
        return Error::NotFound;
    }
    if (!canWrite(*oldParent, user)) {
        return Error::Permission;
    }
    if (find(to)) {
        return Error::Exists;
    }
    // Create the target slot first (checks names, permissions, limits) and swap
    // the subtree into it.
    Node *slot = nullptr;
    if (const auto e = create(to, it->second->directory, user, now, slot);
        e != Error::None) {
        return e;
    }
    Node &source = *it->second;
    slot->data = std::move(source.data);
    slot->mode = source.mode;
    slot->owner = source.owner;
    slot->mtime = source.mtime;
    slot->children = std::move(source.children);
    // `create` added one node for the slot; the old node leaves.
    oldParent = findMutable(parentOf(from)); // Maps may have rehashed: re-find.
    oldParent->children.erase(nameOf(from));
    --nodes_;
    return Error::None;
}

VirtualFs::Error VirtualFs::chmod(const std::string &absolute,
                                  const std::string &user,
                                  std::uint16_t mode) {
    Node *node = findMutable(absolute);
    if (!node) {
        return Error::NotFound;
    }
    if (user != "root" && node->owner != user) {
        return Error::Permission;
    }
    node->mode = mode & 0777;
    return Error::None;
}

void VirtualFs::putDirectory(const std::string &absolute,
                             const std::string &owner,
                             std::uint16_t mode,
                             std::int64_t mtime,
                             bool restyle) {
    Node *node = root_.get();
    std::size_t i = 1;
    while (i < absolute.size() + 1 && absolute != "/") {
        const auto end = absolute.find('/', i);
        const auto name =
            absolute.substr(i, end == std::string::npos ? end : end - i);
        auto it = node->children.find(name);
        if (it == node->children.end()) {
            auto child = std::make_unique<Node>();
            child->directory = true;
            child->owner = owner;
            child->mode = mode;
            child->mtime = mtime;
            it = node->children.emplace(name, std::move(child)).first;
            ++nodes_;
        }
        node = it->second.get();
        if (end == std::string::npos) {
            break;
        }
        i = end + 1;
    }
    if (absolute != "/" && restyle) {
        node->owner = owner;
        node->mode = mode;
    }
}

void VirtualFs::put(const std::string &absolute,
                    const std::string &data,
                    const std::string &owner,
                    std::uint16_t mode,
                    std::int64_t mtime) {
    putDirectory(parentOf(absolute), "root", 0755, mtime, false);
    Node *parent = findMutable(parentOf(absolute));
    auto &slot = parent->children[nameOf(absolute)];
    if (!slot) {
        slot = std::make_unique<Node>();
        ++nodes_;
    }
    else {
        bytes_ -= slot->data.size();
    }
    slot->directory = false;
    slot->data = data;
    slot->owner = owner;
    slot->mode = mode;
    slot->mtime = mtime;
    bytes_ += data.size();
}

} // namespace fakeos
