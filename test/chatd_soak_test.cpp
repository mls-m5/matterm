// Load and soak test of chatd over the plain TCP test listener: many clients,
// a flood, readers that never read, connection churn, and a check that memory
// and file descriptors stay bounded. The default run is short (about 20 s);
// `tools/chatd-soak.sh [seconds]` runs it longer (CHATD_SOAK_SECONDS is the
// length of each phase). Everything is on loopback in one process.
#include "chatdfixture.h"
#include "mls-unit-test/unittest.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <random>

namespace {
using Clock = std::chrono::steady_clock;

double phaseSeconds() {
    if (const char *text = std::getenv("CHATD_SOAK_SECONDS")) {
        const double value = std::strtod(text, nullptr);
        if (value > 0) {
            return value;
        }
    }
    return 3.0;
}
std::size_t residentBytes() {
    std::ifstream statm("/proc/self/statm");
    std::size_t size = 0, resident = 0;
    statm >> size >> resident;
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}
std::size_t openDescriptors() {
    std::size_t count = 0;
    if (DIR *dir = opendir("/proc/self/fd")) {
        while (readdir(dir) != nullptr) {
            ++count;
        }
        closedir(dir);
    }
    return count;
}
chat::ChatRoom::Stats statsOf(Running &r) {
    std::promise<chat::ChatRoom::Stats> promise;
    auto future = promise.get_future();
    net::post(r.io, [&] { promise.set_value(r.server->stats()); });
    return future.get();
}
bool settles(Running &r,
             const std::function<bool(const chat::ChatRoom::Stats &)> &done,
             int milliseconds = 5000) {
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        if (done(statsOf(r))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    return false;
}

// A non-blocking client: `pump` reads what has arrived and notes the end of
// the stream. Writes are blocking with a short timeout.
class Peer {
public:
    Peer(std::uint16_t port, int receiveBuffer = 0) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (receiveBuffer > 0) {
            setsockopt(fd_,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &receiveBuffer,
                       sizeof receiveBuffer);
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_,
                      reinterpret_cast<sockaddr *>(&address),
                      sizeof address) != 0) {
            closed = true;
        }
        timeval tv{0, 200000};
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    ~Peer() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Peer(const Peer &) = delete;
    Peer &operator=(const Peer &) = delete;
    void send(const std::string &line) {
        if (closed) {
            return;
        }
        const std::string data = line + "\n";
        std::size_t at = 0;
        while (at < data.size()) {
            const auto n =
                ::send(fd_, data.data() + at, data.size() - at, MSG_NOSIGNAL);
            if (n <= 0) {
                closed = true;
                return;
            }
            at += static_cast<std::size_t>(n);
        }
    }
    // Reads everything that is there now; returns the number of lines.
    std::size_t pump() {
        std::size_t lines = 0;
        char chunk[4096];
        while (!closed) {
            const auto n = ::recv(fd_, chunk, sizeof chunk, MSG_DONTWAIT);
            if (n > 0) {
                window_.append(chunk, static_cast<std::size_t>(n));
                if (window_.size() > 16384) {
                    window_.erase(0, window_.size() - 8192);
                }
                for (ssize_t i = 0; i < n; ++i) {
                    if (chunk[i] == '\n') {
                        ++lines;
                        if (pending_.compare(0, 5, "ready") == 0) {
                            ready = true;
                        }
                        pending_.clear();
                    }
                    else if (pending_.size() < 16) {
                        pending_ += chunk[i];
                    }
                }
            }
            else if (n == 0) {
                closed = true;
            }
            else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    closed = true;
                }
                break;
            }
        }
        total += lines;
        return lines;
    }
    // Waits for "ready" (or the end of the stream).
    bool join(const std::string &name, int milliseconds = 3000) {
        send("hello\t" + name);
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(milliseconds);
        while (!ready && !closed && Clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return ready;
    }
    // Waits for the server to end the stream, discarding what is still in the
    // pipe.
    bool endsWithin(int milliseconds) {
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(milliseconds);
        while (!closed && Clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return closed;
    }
    // True once `text` has arrived (looks at the most recent kilobytes).
    bool saw(const std::string &text) const {
        return window_.find(text) != std::string::npos;
    }
    bool closed = false, ready = false;
    std::size_t total = 0;

private:
    int fd_ = -1;
    std::string window_;
    std::string pending_;
};

std::string name(const char *prefix, unsigned i) {
    return std::string(prefix) + std::to_string(i);
}
} // namespace

TEST_SUIT_BEGIN(ChatdSoak)

TEST_CASE("Soak: many clients, slow readers, a flood and churn leave memory "
          "and descriptors bounded") {
    const double seconds = phaseSeconds();
    const auto fdsBefore = openDescriptors();
    const auto rssBefore = residentBytes();
    std::size_t rssPeak = rssBefore;
    std::mt19937 random(12345);
    const auto note = [&] { rssPeak = std::max(rssPeak, residentBytes()); };
    const auto during = [&](double s, const std::function<void()> &round) {
        const auto end =
            Clock::now() + std::chrono::milliseconds(int(s * 1000));
        while (Clock::now() < end) {
            round();
            note();
        }
    };
    {
        // The limits of the room are lifted for phases A and B, so that what is
        // measured is the server's machinery, not its rate limits.
        auto open = quick();
        open.room.maxConnections = 300;
        open.room.maxPerSource = 300;
        open.room.maxUnhelloed = 300;
        open.room.connectBurst = 100000;
        open.room.messageBurst = 100000;
        open.room.messageRefill = 1;
        open.room.messagesPerMinute = 100000000;
        open.room.draftForward = 1;
        open.room.draftFloodPerSecond = 100000;
        open.maxPending = 300;
        open.maxPendingPerPeer = 300;
        Running r(open);
        const auto port = r.server->tcpPort();

        // ---- A: a hundred chatting clients -------------------------------
        std::vector<std::unique_ptr<Peer>> crowd;
        for (unsigned i = 0; i < 100; ++i) {
            crowd.push_back(std::make_unique<Peer>(port));
            EXPECT(crowd.back()->join(name("c", i)));
        }
        EXPECT_EQ(statsOf(r).online, std::size_t(100));
        std::size_t rounds = 0;
        during(seconds, [&] {
            for (unsigned i = 0; i < crowd.size(); ++i) {
                if (random() % 10 == 0) {
                    crowd[i]->send("draft\t1\tmessage number " +
                                   std::to_string(rounds));
                    if (random() % 3 == 0) {
                        crowd[i]->send("lock\t1");
                    }
                }
                crowd[i]->pump();
            }
            ++rounds;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        });
        std::size_t dropped = 0, received = 0;
        for (auto &peer : crowd) {
            peer->pump();
            dropped += peer->closed;
            received += peer->total;
        }
        std::cout << "A: " << rounds << " rounds, " << received
                  << " lines received, " << dropped << " dropped\n";
        EXPECT_EQ(dropped, std::size_t(0));
        EXPECT(received > 1000);
        EXPECT(statsOf(r).messages > 10);
        crowd.clear();
        EXPECT(settles(r, [](const auto &s) { return s.connections == 0; }));

        // ---- B: readers that never read ---------------------------------
        std::vector<std::unique_ptr<Peer>> sleepers, talkers;
        for (unsigned i = 0; i < 10; ++i) {
            sleepers.push_back(std::make_unique<Peer>(port, 1024));
            EXPECT(sleepers.back()->join(name("sleeper", i)));
        }
        for (unsigned i = 0; i < 8; ++i) {
            talkers.push_back(std::make_unique<Peer>(port));
            EXPECT(talkers.back()->join(name("talker", i)));
        }
        std::string filler;
        while (filler.size() < 180) {
            filler += "word" + std::to_string(random() % 1000) + " ";
        }
        filler.resize(179);
        filler += 'z';
        unsigned beat = 0;
        during(seconds * 5, [&] {
            ++beat;
            for (auto &talker : talkers) {
                // A new bubble every 20 drafts, locked before it is replaced.
                talker->send("draft\t" + std::to_string(beat / 20 + 1) + "\t" +
                             filler);
                if (beat % 20 == 19) {
                    talker->send("lock\t" + std::to_string(beat / 20 + 1));
                }
                talker->pump();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        });
        unsigned sleepersGone = 0, talkersGone = 0;
        for (auto &sleeper : sleepers) {
            sleepersGone += sleeper->endsWithin(5000);
        }
        for (auto &talker : talkers) {
            talker->pump();
            talkersGone += talker->closed;
        }
        std::cout << "B: " << sleepersGone << "/10 slow readers dropped, "
                  << talkersGone << " of 8 active clients lost\n";
        EXPECT_EQ(sleepersGone, 10u);
        EXPECT_EQ(talkersGone, 0u);
        sleepers.clear();
        talkers.clear();
        EXPECT(settles(r, [](const auto &s) { return s.connections == 0; }));
    }
    {
        // ---- C: a flood with the real limits ----------------------------
        auto real = quick();
        real.room.connectBurst = 40;
        real.room.maxPerSource = 40;
        Running r(real);
        const auto port = r.server->tcpPort();
        Peer watcher(port), sender(port);
        EXPECT(watcher.join("watcher"));
        EXPECT(sender.join("sender"));
        std::vector<std::unique_ptr<Peer>> flood;
        for (unsigned i = 0; i < 5; ++i) {
            flood.push_back(std::make_unique<Peer>(port));
            EXPECT(flood.back()->join(name("flood", i)));
        }
        double worst = 0;
        unsigned probes = 0, seen = 0;
        auto nextProbe = Clock::now();
        std::string junk;
        while (junk.size() < 180) {
            junk += "junk" + std::to_string(random() % 1000) + " ";
        }
        during(seconds * 2, [&] {
            for (auto &peer : flood) {
                for (unsigned i = 0; i < 20; ++i) {
                    peer->send(i % 4 == 0 ? "garbage line " + junk
                                          : "draft\t1\t" + junk);
                }
                peer->pump();
            }
            if (Clock::now() >= nextProbe) {
                nextProbe = Clock::now() + std::chrono::milliseconds(200);
                const auto text = "probe " + std::to_string(probes++);
                const auto start = Clock::now();
                sender.send("draft\t1\t" + text);
                bool found = false;
                while (!found &&
                       Clock::now() - start < std::chrono::seconds(2)) {
                    sender.pump();
                    watcher.pump();
                    found = watcher.saw(text);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (found) {
                    ++seen;
                    worst = std::max(
                        worst,
                        std::chrono::duration<double>(Clock::now() - start)
                            .count());
                }
            }
        });
        unsigned kicked = 0;
        for (auto &peer : flood) {
            peer->pump();
            kicked += peer->closed;
        }
        std::cout << "C: " << probes << " probes, " << seen
                  << " seen, worst delay " << worst << " s, " << kicked
                  << "/5 flooders removed, violations " << statsOf(r).violations
                  << "\n";
        EXPECT(!watcher.closed && !sender.closed);
        EXPECT(seen * 10 >= probes * 9); // Nine in ten probes come through ...
        EXPECT(worst < 1.0);             // ... promptly.
        EXPECT_EQ(kicked, 5u);

        flood.clear();
        EXPECT(!watcher.closed);
        EXPECT(settles(r, [](const auto &s) { return s.connections <= 2; }));
    }
    {
        // ---- D: churn ---------------------------------------------------
        // D1: the real limits. One address may start a few connections, then
        // one every few seconds; the rest are refused cheaply.
        auto real = quick();
        Running r(real);
        unsigned admitted = 0, refused = 0, visits = 0;
        const auto churnEnd =
            Clock::now() + std::chrono::milliseconds(int(seconds * 1000));
        for (; Clock::now() < churnEnd; ++visits) {
            Peer visitor(r.server->tcpPort());
            visitor.send("hello\tv" + std::to_string(visits % 1000));
            visitor.join("v" + std::to_string(visits % 1000), 100);
            (visitor.ready ? admitted : refused) += 1;
            note();
        }
        std::cout << "D1: " << visits << " visits, " << admitted
                  << " admitted, " << refused << " refused\n";
        EXPECT(admitted <= 5 + 1 + unsigned(seconds / 6.0 + 1));
        EXPECT(statsOf(r).bans == 0);
        EXPECT(settles(r, [](const auto &s) { return s.connections == 0; }));
    }
    {
        // D2: limits lifted, every visit is admitted: join, leave, repeat.
        auto open = quick();
        open.room.connectBurst = 100000;
        open.room.connectRefill = 1;
        open.room.maxPerSource = 100;
        Running r(open);
        Peer stayer(r.server->tcpPort());
        EXPECT(stayer.join("stayer"));
        unsigned admitted = 0, visits = 0;
        const auto churnEnd =
            Clock::now() + std::chrono::milliseconds(int(seconds * 1000));
        for (; Clock::now() < churnEnd; ++visits) {
            Peer visitor(r.server->tcpPort());
            admitted += visitor.join("w" + std::to_string(visits % 1000), 500);
            stayer.pump();
            note();
        }
        std::cout << "D2: " << visits << " visits, " << admitted
                  << " admitted\n";
        EXPECT(admitted * 10 >= visits * 9);
        EXPECT(!stayer.closed);
        EXPECT(settles(r, [](const auto &s) { return s.connections == 1; }));
        EXPECT_EQ(statsOf(r).online, std::size_t(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto rssAfter = residentBytes();
    const auto fdsAfter = openDescriptors();
    std::cout << "memory: " << rssBefore / 1024 / 1024 << " MiB before, peak "
              << rssPeak / 1024 / 1024 << " MiB, after "
              << rssAfter / 1024 / 1024 << " MiB; descriptors " << fdsBefore
              << " -> " << fdsAfter << "\n";
    EXPECT(rssPeak < rssBefore + (64u << 20));
    EXPECT(rssAfter < rssBefore + (32u << 20));
    EXPECT(fdsAfter <= fdsBefore + 4);
}

TEST_SUIT_END
