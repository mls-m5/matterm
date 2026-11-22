#pragma once

// MessageChannel over a Unix domain socket, for the local daemon
// (docs/tasks/daemon.md, docs/TABS_PLAN.md section 10). Each message is a 4-byte
// big-endian length (1..16384) followed by the bytes a WebSocket binary message
// carries (docs/PROTOCOL.md). Anything else closes the connection with code 1002.
//
// Non-blocking and single-threaded: the owner calls poll() from its frame or timer
// loop (and may wait for fd() to become readable, writable while hasPendingOutput()).
// Callbacks run inside poll(), never inside send() or close(), so they may call
// both. `opened` is delivered by the first poll(), `closed` exactly once by a poll()
// after the connection ended (peer EOF at a message boundary 1000, mid-message or an
// error 1006, a framing violation 1002, or the code given to close()).
#include "messagechannel.h"

#include <cstddef>
#include <stdexcept>
#include <string>

class UnixConnectError : public std::runtime_error {
public:
    UnixConnectError(const std::string &what, int error)
        : std::runtime_error(what)
        , error_(error) {}
    // errno: ENOENT or ECONNREFUSED mean "no daemon is listening there".
    int error() const noexcept {
        return error_;
    }

private:
    int error_;
};

class UnixSocketChannel final : public MessageChannel {
public:
    static constexpr std::size_t kMaxMessage = 16384;
    static constexpr std::size_t kMaxQueued = 1 << 20; // Unsent bytes before send() says no.

    // Connects to the socket at `path` (it must exist and be listening); throws
    // UnixConnectError. A path longer than sockaddr_un holds is an error too.
    explicit UnixSocketChannel(const std::string &path);
    // Takes over an already connected stream socket (tests, socketpair, accept()).
    explicit UnixSocketChannel(int fd);
    ~UnixSocketChannel() override;
    UnixSocketChannel(const UnixSocketChannel &) = delete;
    UnixSocketChannel &operator=(const UnixSocketChannel &) = delete;

    void setEvents(Events events) override;
    // False for an empty or too long message, a closed channel or a full queue.
    bool send(std::string_view bytes) override;
    void close(int code) override;

    // Writes what is queued, reads what has arrived, delivers events.
    void poll();

    int fd() const noexcept {
        return fd_;
    }
    bool isOpen() const noexcept {
        return fd_ >= 0;
    }
    bool hasPendingOutput() const noexcept {
        return outPos_ < out_.size();
    }

private:
    void adopt(int fd);
    void flush();
    void readAvailable();
    void process();
    void endConnection(int code);

    int fd_ = -1;
    Events events_;
    bool openedReported_ = false;
    int pendingClose_ = 0; // 0: none yet.
    bool closeReported_ = false;
    std::string in_;
    std::size_t inPos_ = 0;
    std::string out_;
    std::size_t outPos_ = 0;
};

// Where the local daemon listens by default: $XDG_RUNTIME_DIR/matterm/daemon.sock,
// else /tmp/matterm-<uid>/daemon.sock. (The daemon creates and protects the
// directory; this only names the path.)
std::string defaultDaemonSocketPath();
