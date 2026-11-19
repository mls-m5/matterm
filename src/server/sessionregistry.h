#pragma once
#include "ptysessionhost.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace server {
struct RegistryConfig {
    bool persistentSessions = false;
    std::size_t replayBufferBytes = 0;
    std::size_t replayTotalBytes = 64u << 20;
    unsigned maxSessions = 16;
    unsigned maxDetachedSessions = 16;
    protocol::Time detachedTimeout{3600000};
};
// Single io_context-thread owner. Authentication/Origin checks happen in the
// HTTP layer before any call to attach. All ids belong to the single owner.
class SessionRegistry {
public:
    struct Transport {
        std::function<void(std::string)> send;
        std::function<void(int)> close;
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
    void closeAll(protocol::Time now);
    std::size_t size() const;
    std::size_t detachedSize() const;
    std::size_t replayCost() const;
    std::size_t tombstoneSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace server
