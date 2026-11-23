#include "sessionregistry.h"
#include "auth.h"
#include "replayring.h"
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <sodium.h>
#include <stdexcept>
#include <tuple>

namespace server {
namespace {
using namespace protocol;
void emit(const SessionRegistry::Transport &transport,
          const ServerMessage &message) {
    std::string bytes;
    if (encode(message, bytes) && transport.send)
        transport.send(std::move(bytes));
}
void reject(const SessionRegistry::Transport &transport,
            ErrorCode code,
            const char *reason) {
    emit(transport, Error{code, reason});
    if (transport.close)
        transport.close(closeCodeFor(code));
}
bool validId(const std::string &id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::string newId() {
    std::array<unsigned char, 16> bytes{};
    std::array<char, 33> hex{};
    randombytes_buf(bytes.data(), bytes.size());
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}
struct Entry : SessionHost {
    std::string id;
    ReplayRing ring;
    PtySessionHost pty;
    std::shared_ptr<ServerSession> session;
    SessionRegistry::Transport transport;
    std::uint64_t generation = 0, replayAt = 0;
    bool persistent, attached = false, started = false, closing = false,
                     ended = false;
    bool tombstone = false;
    bool shared = false;
    std::string name;
    int exitCode = -1, exitSignal = 0;
    Time detachedAt{};
    explicit Entry(std::string identifier,
                   bool keep,
                   std::size_t budget,
                   ProcessConfig process)
        : id(std::move(identifier))
        , ring(keep ? budget : 0)
        , pty(std::move(process), {}, {})
        , persistent(keep) {}
    void send(std::string bytes) override {
        const auto sink = transport.send;
        if (attached && sink)
            sink(std::move(bytes));
    }
    void closeTransport(int code) override {
        if (!started)
            ended = true;
        const auto sink = transport.close;
        if (attached && sink)
            sink(code);
    }
    bool startProcess(std::uint16_t columns, std::uint16_t rows) override {
        if (started)
            return true;
        started = pty.startProcess(columns, rows);
        return started;
    }
    bool writeInput(std::string_view bytes) override {
        return pty.writeInput(bytes);
    }
    void resizeProcess(std::uint16_t c,
                       std::uint16_t r,
                       std::uint16_t x,
                       std::uint16_t y) override {
        pty.resizeProcess(c, r, x, y);
    }
    void terminateProcess() override {
        closing = true;
        ring.disable();
        pty.terminateProcess();
    }
    void detach(Time now) {
        attached = false;
        if (transport.release)
            transport.release();
        transport = {};
        detachedAt = now;
        auto old = session;
        if (!persistent || closing) {
            if (old)
                old->onTransportClosed(now);
            else
                terminateProcess();
        }
        session.reset();
    }
    void stop(Time now) {
        closing = true;
        persistent = false;
        ring.disable();
        if (auto current = session)
            current->shutdown(now);
        pty.terminateProcess();
    }
    void pump(Time now, std::uint64_t serial) {
        if (ended)
            return;
        auto current = session;
        if (attached && current) {
            current->tick(now);
            if (replayAt < ring.end() &&
                current->state() == ServerSession::State::Running) {
                if (current->outputBudget()) {
                    const auto bytes = ring.read(
                        replayAt,
                        std::min(current->outputBudget(), maxOutputData));
                    if (!bytes || bytes->empty()) {
                        const auto previous = transport;
                        detach(now); // Ring evicted during backpressure: retry,
                                     // never send a gap.
                        reject(previous,
                               ErrorCode::ResumeUnsupported,
                               "session unavailable");
                        return;
                    }
                    current->onProcessOutput(*bytes, now);
                    replayAt += bytes->size();
                }
                return; // Live reads wait until replay has been fully queued.
            }
        }
        const auto budget = attached && current ? current->outputBudget()
                            : closing           ? std::size_t(0)
                                                : maxOutputData;
        pty.pump(budget,
                 now,
                 {[&](std::string_view bytes) {
                      ring.append(now, bytes, serial);
                      if (attached && current)
                          current->onProcessOutput(bytes, now);
                      replayAt = ring.end();
                  },
                  [&](int code, int signal, bool drained) {
                      exitCode = code;
                      exitSignal = signal;
                      tombstone = persistent && !closing && drained;
                      ended = true;
                      ring.clear();
                      if (attached && current && drained)
                          current->onProcessExit(code, signal, now);
                      else if (attached && transport.close)
                          transport.close(1000);
                  }});
    }
};
} // namespace
struct SessionRegistry::Impl {
    RegistryConfig config;
    ProcessConfig process;
    SessionConfig protocolConfig;
    std::map<std::string, std::shared_ptr<Entry>> entries;
    struct Tombstone {
        std::string id;
        std::uint64_t end;
        int code, signal;
    };
    std::deque<Tombstone> tombstones;
    std::uint64_t serial = 0;
    Impl(RegistryConfig cfg, ProcessConfig proc, SessionConfig proto)
        : config(cfg)
        , process(std::move(proc))
        , protocolConfig(std::move(proto)) {
        initializeCrypto();
        if (!config.maxSessions ||
            config.maxDetachedSessions > config.maxSessions ||
            config.detachedTimeout.count() < 0 ||
            config.replayBufferBytes > config.replayTotalBytes)
            throw std::invalid_argument("Invalid session registry limits");
    }
    std::size_t total() const {
        std::size_t cost = 0;
        for (const auto &[id, entry] : entries)
            cost += entry->ring.cost();
        return cost;
    }
    void trim() {
        while (total() > config.replayTotalBytes) {
            std::shared_ptr<Entry> oldest;
            for (const auto &[id, entry] : entries) {
                if (!entry->ring.cost())
                    continue;
                if (!oldest || std::make_tuple(entry->ring.oldestTime(),
                                               entry->ring.oldestSerial()) <
                                   std::make_tuple(oldest->ring.oldestTime(),
                                                   oldest->ring.oldestSerial()))
                    oldest = entry;
            }
            if (!oldest)
                break;
            oldest->ring.dropOldest();
        }
    }
    std::shared_ptr<Entry> find(const Attachment &a) {
        const auto found = entries.find(a.id);
        return found != entries.end() &&
                       found->second->generation == a.generation &&
                       found->second->attached
                   ? found->second
                   : nullptr;
    }
};
SessionRegistry::SessionRegistry(RegistryConfig cfg,
                                 ProcessConfig proc,
                                 SessionConfig proto)
    : impl_(std::make_unique<Impl>(cfg, std::move(proc), std::move(proto))) {}
SessionRegistry::~SessionRegistry() = default;
std::optional<SessionRegistry::Attachment> SessionRegistry::attach(
    const Hello &hello, Transport transport, Time now) {
    std::string checked;
    if (!encode(ClientMessage{hello}, checked) || hello.version == 0) {
        reject(transport, ErrorCode::BadMessage, "invalid Hello");
        return {};
    }
    const bool resume = !hello.sessionId.empty();
    const bool capable = impl_->config.persistentSessions &&
                         impl_->config.replayBufferBytes > 0 &&
                         (hello.features & featureReplay);
    if ((resume && (!capable || !validId(hello.sessionId))) ||
        (!resume && hello.resumeOffset)) {
        reject(transport, ErrorCode::ResumeUnsupported, "session unavailable");
        return {};
    }
    std::shared_ptr<Entry> entry;
    std::uint64_t start = 0;
    std::uint8_t flags = 0;
    if (resume) {
        const auto found = impl_->entries.find(hello.sessionId);
        if (found == impl_->entries.end()) {
            const auto dead = std::find_if(
                impl_->tombstones.begin(),
                impl_->tombstones.end(),
                [&](const auto &t) { return t.id == hello.sessionId; });
            if (dead != impl_->tombstones.end() &&
                hello.resumeOffset <= dead->end) {
                if (transport.admit && !transport.admit(false)) {
                    reject(
                        transport, ErrorCode::LimitExceeded, "upgrade limit");
                    return {};
                }
                emit(transport,
                     Welcome{1, dead->id, dead->end, 3, featureReplay});
                emit(transport, Exit{dead->code, dead->signal});
                if (transport.close)
                    transport.close(1000);
                return Attachment{dead->id, 0};
            }
            reject(
                transport, ErrorCode::ResumeUnsupported, "session unavailable");
            return {};
        }
        entry = found->second;
        if (!entry->persistent || entry->closing || entry->ended) {
            reject(
                transport, ErrorCode::ResumeUnsupported, "session unavailable");
            return {};
        }
        if (hello.resumeOffset > entry->ring.end()) {
            reject(transport, ErrorCode::BadMessage, "invalid resume offset");
            return {};
        }
        start = hello.resumeOffset;
        flags = welcomeResumed;
        if (!start || start < entry->ring.oldest()) {
            start = entry->ring.safeStart();
            flags |= welcomeOutputLost;
        }
    }
    else {
        if (impl_->entries.size() >= impl_->config.maxSessions) {
            reject(transport, ErrorCode::LimitExceeded, "session limit");
            return {};
        }
        std::string id;
        do {
            id = newId();
        } while (impl_->entries.count(id) ||
                 std::any_of(impl_->tombstones.begin(),
                             impl_->tombstones.end(),
                             [&](const auto &t) { return t.id == id; }));
        auto process = impl_->process;
        process.extraEnvironment.push_back("MATTERM_SESSION=" + id);
        entry = std::make_shared<Entry>(
            id, capable, impl_->config.replayBufferBytes, std::move(process));
    }
    if (entry->generation == std::numeric_limits<std::uint64_t>::max()) {
        reject(transport, ErrorCode::Internal, "attachment limit");
        return {};
    }
    const auto previous = entry->attached ? entry->transport : Transport{};
    const bool transfer =
        previous.release && previous.authGeneration == transport.authGeneration;
    if (transport.admit && !transport.admit(transfer)) {
        reject(transport, ErrorCode::LimitExceeded, "upgrade limit");
        return {};
    }
    if (resume) {
        // After the lease and before the old attachment is evicted: a refused
        // attach must not touch the terminal. A one-cell toggle forces SIGWINCH
        // even when the requested dimensions are unchanged.
        try {
            entry->pty.resizeProcess(
                hello.columns == 1 ? 2 : hello.columns - 1, hello.rows, 0, 0);
            entry->pty.resizeProcess(hello.columns, hello.rows, 0, 0);
        }
        catch (const std::exception &) {
            if (transport.release)
                transport.release();
            reject(transport, ErrorCode::Internal, "could not attach");
            return {};
        }
    }
    if (!resume)
        impl_->entries.emplace(entry->id, entry);
    ++entry->generation; // Fence old events before closing its transport.
    entry->transport = std::move(transport);
    entry->attached = true;
    entry->replayAt = start;
    auto config = impl_->protocolConfig;
    config.sessionId = entry->id;
    config.startOffset = start;
    config.features =
        (capable ? featureReplay : 0) | impl_->config.extraFeatures;
    config.welcomeFlags = flags;
    config.resume = resume;
    entry->session = std::make_shared<ServerSession>(config, *entry);
    if (previous.release)
        previous.release();
    if (previous.send || previous.close)
        reject(previous, ErrorCode::Replaced, "session taken over");
    auto current = entry->session;
    current->start(now);
    current->onMessage(checked, now);
    return Attachment{entry->id, entry->generation};
}
void SessionRegistry::onMessage(const Attachment &a,
                                std::string_view bytes,
                                Time now) {
    if (const auto entry = impl_->find(a)) {
        const auto current = entry->session;
        if (current)
            current->onMessage(bytes, now);
    }
}
void SessionRegistry::detach(const Attachment &a, Time now) {
    if (const auto entry = impl_->find(a)) {
        entry->detach(now);
        if (!entry->closing &&
            detachedSize() > impl_->config.maxDetachedSessions)
            entry->stop(now);
    }
}
void SessionRegistry::fail(const Attachment &a, Time now) {
    if (const auto entry = impl_->find(a)) {
        entry->terminateProcess();
        if (const auto current = entry->session)
            current->onTransportClosed(now);
    }
}
void SessionRegistry::tick(Time now) {
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        auto entry = it->second;
        if (!entry->attached && !entry->closing &&
            impl_->config.detachedTimeout.count() &&
            now - entry->detachedAt >= impl_->config.detachedTimeout)
            entry->stop(now);
        entry->pump(now, ++impl_->serial);
        impl_->trim();
        if (!entry->ended) {
            ++it;
            continue;
        }
        if (entry->tombstone) {
            impl_->tombstones.push_back({entry->id,
                                         entry->ring.end(),
                                         entry->exitCode,
                                         entry->exitSignal});
            while (impl_->tombstones.size() > impl_->config.maxSessions)
                impl_->tombstones.pop_front();
        }
        it = impl_->entries.erase(it);
    }
}
std::optional<Time> SessionRegistry::nextDeadline() const {
    std::optional<Time> result;
    const auto consider = [&](Time value) {
        if (!result || value < *result)
            result = value;
    };
    for (const auto &[id, entry] : impl_->entries) {
        if (entry->attached && entry->session) {
            if (const auto deadline = entry->session->nextDeadline())
                consider(*deadline);
        }
        else if (!entry->closing && impl_->config.detachedTimeout.count()) {
            consider(entry->detachedAt + impl_->config.detachedTimeout);
        }
    }
    return result;
}
void SessionRegistry::closeAll(Time now) {
    impl_->tombstones.clear();
    for (auto &[id, entry] : impl_->entries)
        entry->stop(now);
}
std::vector<SessionRegistry::Info> SessionRegistry::list() const {
    std::vector<Info> out;
    for (const auto &[id, entry] : impl_->entries) {
        if (entry->closing || entry->ended)
            continue;
        out.push_back({id, entry->attached, entry->shared, entry->ring.end(), entry->name});
    }
    return out;
}
bool SessionRegistry::setShared(const std::string &id, bool shared) {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || found->second->closing ||
        found->second->ended)
        return false;
    found->second->shared = shared;
    return true;
}
bool SessionRegistry::setName(const std::string &id, const std::string &name) {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || found->second->closing || found->second->ended)
        return false;
    found->second->name = name;
    return true;
}
std::optional<std::string> SessionRegistry::create(
    const std::string &workingDirectory,
    std::uint16_t columns,
    std::uint16_t rows,
    Time now,
    const std::vector<std::string> &command,
    const std::string &name) {
    if (!impl_->config.persistentSessions || impl_->config.replayBufferBytes == 0 ||
        impl_->entries.size() >= impl_->config.maxSessions || columns == 0 || rows == 0 ||
        columns > maxDimension || rows > maxDimension)
        return std::nullopt;
    std::string id;
    do {
        id = newId();
    } while (impl_->entries.count(id) ||
             std::any_of(impl_->tombstones.begin(),
                         impl_->tombstones.end(),
                         [&](const auto &t) { return t.id == id; }));
    auto process = impl_->process;
    process.workingDirectory = workingDirectory;
    if (!command.empty())
        process.arguments = command;
    process.extraEnvironment.push_back("MATTERM_SESSION=" + id);
    auto entry = std::make_shared<Entry>(id, true, impl_->config.replayBufferBytes, std::move(process));
    if (!entry->startProcess(columns, rows))
        return std::nullopt;
    entry->detachedAt = now;
    entry->name = name;
    impl_->entries.emplace(id, entry);
    if (detachedSize() > impl_->config.maxDetachedSessions) {
        entry->stop(now);
        return std::nullopt;
    }
    return id;
}
std::optional<SessionRegistry::ProcessIds> SessionRegistry::processIds(const std::string &id) const {
    const auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || found->second->closing || found->second->ended ||
        !found->second->started)
        return std::nullopt;
    return ProcessIds{found->second->pty.childPid(), found->second->pty.foregroundProcessGroup()};
}
std::size_t SessionRegistry::size() const {
    return impl_->entries.size();
}
std::size_t SessionRegistry::detachedSize() const {
    return std::count_if(
        impl_->entries.begin(), impl_->entries.end(), [](const auto &pair) {
            return !pair.second->attached && !pair.second->closing;
        });
}
std::size_t SessionRegistry::replayCost() const {
    return impl_->total();
}
std::size_t SessionRegistry::tombstoneSize() const {
    return impl_->tombstones.size();
}
} // namespace server
