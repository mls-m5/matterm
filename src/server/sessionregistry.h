#pragma once
#include "ptysessionhost.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace server {
struct RegistryConfig {
    bool persistentSessions = false;
    std::size_t replayBufferBytes = 0;
    std::size_t replayTotalBytes = 64u << 20;
    unsigned maxSessions = 16;
    unsigned maxDetachedSessions = 16;
    protocol::Time detachedTimeout{3600000}; // 0: a detached session never expires
    // Feature bits this host offers besides Replay (a client gets those it asked for).
    // The local daemon sets its "daemon" bit here; the web server leaves it 0.
    std::uint32_t extraFeatures = 0;
};
// Single io_context-thread owner. Authentication/Origin checks happen in the
// HTTP layer before any call to attach. All ids belong to the single owner.
class SessionRegistry {
public:
    struct Transport {
        std::function<void(std::string)> send;
        std::function<void(int)> close;
        // HTTP layer lease callbacks; CPU-only users may leave these empty.
        // transfer is true only for a same-login live attachment replacement.
        std::function<bool(bool transfer)> admit;
        std::function<void()> release;
        std::uint64_t authGeneration = 0;
    };
    struct Attachment {
        std::string id;
        std::uint64_t generation = 0;
    };
    SessionRegistry(RegistryConfig config,
                    ProcessConfig process,
                    protocol::SessionConfig protocolConfig);
    ~SessionRegistry();
    std::optional<Attachment> attach(const protocol::Hello &hello,
                                     Transport transport,
                                     protocol::Time now);
    void onMessage(const Attachment &, std::string_view, protocol::Time now);
    void detach(const Attachment &, protocol::Time now);
    void fail(const Attachment &, protocol::Time now);
    void tick(protocol::Time now);
    std::optional<protocol::Time> nextDeadline() const;
    void closeAll(protocol::Time now);
    // What a control client may know about a session. No terminal contents.
    struct Info {
        std::string id;
        bool attached = false;
        bool shared = false;      // The owner marked it as visible to the web server (docs/TABS_PLAN.md s11).
        std::uint64_t outputEnd = 0; // Absolute offset of the end of the replay ring.
        std::string name;            // Given by the owner (daemon: SetName); empty: none.
    };
    std::vector<Info> list() const;
    // Marks a live session shared or private. False for an unknown or ending session.
    bool setShared(const std::string &id, bool shared);
    // Names a live session (the caller validated the text). False for an unknown or ending one.
    bool setName(const std::string &id, const std::string &name);
    // Ends a session's program as a client's Close does (hang up, kill after
    // the grace) and forgets the session when it is gone; an attached client
    // sees the program end. False for an unknown or ending one.
    bool end(const std::string &id, protocol::Time now);
    // Starts a persistent session nobody is attached to yet (the client then attaches
    // with Hello{sessionId}). `workingDirectory` empty: the daemon's own. Nullopt when the
    // registry is full, cannot persist sessions, or the process did not start.
    std::optional<std::string> create(
        const std::string &workingDirectory,
        std::uint16_t columns,
        std::uint16_t rows,
        protocol::Time now,
        const std::vector<std::string> &command = {},
        const std::string &name = {});
    // Process ids of a live session, for asking /proc what it runs.
    struct ProcessIds {
        int shell = 0;
        int foreground = 0;
    };
    std::optional<ProcessIds> processIds(const std::string &id) const;
    std::size_t size() const;
    std::size_t detachedSize() const;
    std::size_t replayCost() const;
    std::size_t tombstoneSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace server
