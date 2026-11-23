#include "unixchannel.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
void makeNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    const int fdFlags = ::fcntl(fd, F_GETFD, 0);
    if (fdFlags >= 0) {
        ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
    }
}
} // namespace

UnixSocketChannel::UnixSocketChannel(const std::string &path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof address.sun_path ||
        path.find('\0') != std::string::npos) {
        throw UnixConnectError("socket path is empty or too long", ENAMETOOLONG);
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw UnixConnectError(std::string("socket: ") + std::strerror(errno), errno);
    }
    makeNonBlocking(fd);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
        const int error = errno;
        ::close(fd);
        throw UnixConnectError(
            "cannot connect to " + path + ": " + std::strerror(error), error);
    }
    // The daemon must belong to us: a socket planted by another user (for instance under
    // /tmp) would get everything the terminal receives and the owner types.
    ucred cred{};
    socklen_t credLength = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLength) != 0 || cred.uid != ::geteuid()) {
        ::close(fd);
        throw UnixConnectError("the socket at " + path + " belongs to another user", EACCES);
    }
    adopt(fd);
}

UnixSocketChannel::UnixSocketChannel(int fd) {
    makeNonBlocking(fd);
    adopt(fd);
}

void UnixSocketChannel::adopt(int fd) {
    fd_ = fd;
}

UnixSocketChannel::~UnixSocketChannel() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void UnixSocketChannel::setEvents(Events events) {
    events_ = std::move(events);
}

bool UnixSocketChannel::send(std::string_view bytes) {
    if (fd_ < 0 || bytes.empty() || bytes.size() > kMaxMessage ||
        (out_.size() - outPos_) + 4 + bytes.size() > kMaxQueued) {
        return false;
    }
    const auto n = std::uint32_t(bytes.size());
    out_.push_back(char((n >> 24) & 0xff));
    out_.push_back(char((n >> 16) & 0xff));
    out_.push_back(char((n >> 8) & 0xff));
    out_.push_back(char(n & 0xff));
    out_.append(bytes);
    flush(); // As much as the socket takes now; poll() does the rest.
    return true;
}

void UnixSocketChannel::flush() {
    while (fd_ >= 0 && outPos_ < out_.size()) {
        const auto written = ::send(fd_,
                                    out_.data() + outPos_,
                                    out_.size() - outPos_,
                                    MSG_NOSIGNAL);
        if (written > 0) {
            outPos_ += std::size_t(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        endConnection(1006);
        return;
    }
    if (outPos_ == out_.size()) {
        out_.clear();
        outPos_ = 0;
    }
    else if (outPos_ > (1u << 16)) {
        out_.erase(0, outPos_);
        outPos_ = 0;
    }
}

void UnixSocketChannel::endConnection(int code) {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (pendingClose_ == 0) {
        pendingClose_ = code;
    }
}

void UnixSocketChannel::close(int code) {
    if (fd_ >= 0) {
        flush(); // Best effort: a Close message may still be queued.
    }
    endConnection(code != 0 ? code : 1000);
}

void UnixSocketChannel::poll() {
    if (!openedReported_ && fd_ >= 0) {
        openedReported_ = true;
        if (events_.opened) {
            events_.opened();
        }
    }
    if (fd_ >= 0) {
        flush();
    }
    if (fd_ >= 0) {
        readAvailable();
    }
    if (pendingClose_ != 0 && !closeReported_) {
        closeReported_ = true;
        in_.clear();
        inPos_ = 0;
        out_.clear();
        outPos_ = 0;
        if (events_.closed) {
            events_.closed(pendingClose_);
        }
    }
}

void UnixSocketChannel::readAvailable() {
    char buffer[65536];
    // Bounded per call, so a flood cannot starve the frame loop.
    for (int round = 0; round < 16 && fd_ >= 0; ++round) {
        const auto got = ::read(fd_, buffer, sizeof buffer);
        if (got > 0) {
            in_.append(buffer, std::size_t(got));
            process();
            continue;
        }
        if (got == 0) {
            // The peer is gone: a clean end only between messages.
            process();
            endConnection(fd_ >= 0 && inPos_ >= in_.size() ? 1000 : 1006);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            endConnection(1006);
        }
        return;
    }
}

void UnixSocketChannel::process() {
    while (fd_ >= 0 && in_.size() - inPos_ >= 4) {
        const auto *p = reinterpret_cast<const unsigned char *>(in_.data() + inPos_);
        const std::size_t length = (std::size_t(p[0]) << 24) | (std::size_t(p[1]) << 16) |
                                   (std::size_t(p[2]) << 8) | std::size_t(p[3]);
        if (length == 0 || length > kMaxMessage) {
            endConnection(1002); // Not our framing: nothing more is trusted.
            return;
        }
        if (in_.size() - inPos_ < 4 + length) {
            break;
        }
        const std::string_view message(in_.data() + inPos_ + 4, length);
        inPos_ += 4 + length;
        if (events_.message) {
            events_.message(message); // May call send() or close(); in_ stays intact.
        }
    }
    if (inPos_ > 0 && (inPos_ == in_.size() || inPos_ > (1u << 16))) {
        in_.erase(0, inPos_);
        inPos_ = 0;
    }
}

std::string defaultDaemonSocketPath() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime == '/') {
        return std::string(runtime) + "/matterm/daemon.sock";
    }
    return "/tmp/matterm-" + std::to_string(unsigned(::getuid())) + "/daemon.sock";
}
