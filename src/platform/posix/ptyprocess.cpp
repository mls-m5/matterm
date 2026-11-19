#include "ptyprocess.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {
struct Fd {
    int value = -1;
    ~Fd() {
        close();
    }
    void close() noexcept {
        if (value >= 0) {
            ::close(value);
            value = -1;
        }
    }
};

[[noreturn]] void fail(const char *operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void dimensions(int columns, int rows) {
    if (columns <= 0 || columns > USHRT_MAX || rows <= 0 || rows > USHRT_MAX) {
        throw std::invalid_argument("PTY dimensions must be in 1..65535");
    }
}

std::string executable(const std::string &command) {
    if (command.find('/') != std::string::npos) {
        return command;
    }
    const char *pathEnv = std::getenv("PATH");
    const std::string path = pathEnv ? pathEnv : "/usr/local/bin:/usr/bin:/bin";
    for (std::size_t begin = 0;;) {
        const auto end = path.find(':', begin);
        const auto directory = path.substr(begin, end - begin);
        const auto candidate =
            (directory.empty() ? "." : directory) + "/" + command;
        struct stat info {};
        if (::access(candidate.c_str(), X_OK) == 0 &&
            ::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            return candidate;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    throw std::system_error(ENOENT, std::generic_category(), command);
}

std::vector<std::string> environment(const std::string &term) {
    std::vector<std::string> result;
    for (char **entry = environ; entry && *entry; ++entry) {
        const std::string value = *entry;
        if (value.compare(0, 5, "TERM=") == 0 ||
            value.compare(0, 10, "COLORTERM=") == 0 ||
            value.compare(0, 8, "COLUMNS=") == 0 ||
            value.compare(0, 6, "LINES=") == 0) {
            continue;
        }
        result.push_back(value);
    }
    result.push_back("TERM=" + term);
    return result;
}
} // namespace

struct PtyProcess::Impl {
    Fd master;
    pid_t child = -1;
    std::optional<ExitStatus> status;

    std::optional<ExitStatus> reap() noexcept {
        if (status || child <= 0) {
            return status;
        }
        int value = 0;
        const auto result = ::waitpid(child, &value, WNOHANG);
        if (result == child) {
            status = ExitStatus{WIFEXITED(value) ? WEXITSTATUS(value) : -1,
                                WIFSIGNALED(value) ? WTERMSIG(value) : 0};
        }
        else if (result < 0 && errno == ECHILD) {
            status = ExitStatus{};
        }
        return status;
    }
    void terminate() noexcept {
        master.close();
        if (child <= 0 || reap()) {
            return;
        }
        ::kill(-child, SIGHUP);
        ::kill(child, SIGHUP);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!reap() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (status) {
            return;
        }
        ::kill(-child, SIGKILL);
        ::kill(child, SIGKILL);
        int value = 0;
        pid_t result;
        do {
            result = ::waitpid(child, &value, 0);
        } while (result < 0 && errno == EINTR);
        if (result == child) {
            status = ExitStatus{WIFEXITED(value) ? WEXITSTATUS(value) : -1,
                                WIFSIGNALED(value) ? WTERMSIG(value) : 0};
        }
    }
    ~Impl() {
        terminate();
    }

    void start(const std::vector<std::string> &arguments,
               int columns,
               int rows,
               const std::string &term) {
        dimensions(columns, rows);
        if (arguments.empty() || arguments[0].empty() || term.empty() ||
            term.find('\0') != std::string::npos) {
            throw std::invalid_argument(
                "PTY requires a program and valid TERM");
        }
        for (const auto &argument : arguments) {
            if (argument.find('\0') != std::string::npos) {
                throw std::invalid_argument("NUL in process argument");
            }
        }
        const auto path = executable(arguments[0]);
        auto env = environment(term);
        std::vector<char *> argv, envp;
        for (const auto &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        for (auto &entry : env) {
            envp.push_back(entry.data());
        }
        argv.push_back(nullptr);
        envp.push_back(nullptr);
        int descriptors[2];
        if (::pipe2(descriptors, O_CLOEXEC) < 0) {
            fail("pipe2");
        }
        Fd errorRead{descriptors[0]}, errorWrite{descriptors[1]};
        winsize size{};
        size.ws_col = static_cast<unsigned short>(columns);
        size.ws_row = static_cast<unsigned short>(rows);
        child = ::forkpty(&master.value, nullptr, nullptr, &size);
        if (child < 0) {
            fail("forkpty");
        }
        if (child == 0) {
            ::close(errorRead.value);
            // All allocation/environment/PATH preparation happened before fork.
            ::execve(path.c_str(), argv.data(), envp.data());
            const int error = errno;
            const auto *data = reinterpret_cast<const char *>(&error);
            std::size_t sent = 0;
            while (sent < sizeof(error)) {
                const auto count = ::write(
                    errorWrite.value, data + sent, sizeof(error) - sent);
                if (count > 0) {
                    sent += std::size_t(count);
                }
                else if (count < 0 && errno == EINTR) {
                    continue;
                }
                else {
                    break;
                }
            }
            ::_exit(127);
        }
        errorWrite.close();
        int childError = 0;
        std::size_t received = 0;
        while (received < sizeof(childError)) {
            const auto count =
                ::read(errorRead.value,
                       reinterpret_cast<char *>(&childError) + received,
                       sizeof(childError) - received);
            if (count == 0) {
                break;
            } // CLOEXEC closed the pipe: exec succeeded.
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("exec status read");
            }
            received += std::size_t(count);
        }
        if (received != 0) {
            throw std::system_error(received == sizeof(childError) ? childError
                                                                   : EIO,
                                    std::generic_category(),
                                    "execve");
        }
        const int flags = ::fcntl(master.value, F_GETFL);
        if (flags < 0 ||
            ::fcntl(master.value, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(master.value, F_SETFD, FD_CLOEXEC) < 0) {
            fail("PTY fcntl");
        }
    }
};

PtyProcess::PtyProcess(const std::vector<std::string> &arguments,
                       int columns,
                       int rows,
                       const std::string &term)
    : impl_(std::make_unique<Impl>()) {
    impl_->start(arguments, columns, rows, term);
}
PtyProcess::~PtyProcess() = default;
PtyProcess::ReadResult PtyProcess::read(char *buffer, std::size_t capacity) {
    if (capacity == 0 || !buffer) {
        throw std::invalid_argument("PTY read requires a buffer");
    }
    if (closed()) {
        return {ReadState::Closed, 0};
    }
    ssize_t count;
    do {
        count = ::read(impl_->master.value, buffer, capacity);
    } while (count < 0 && errno == EINTR);
    if (count > 0) {
        return {ReadState::Data, std::size_t(count)};
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return {ReadState::WouldBlock, 0};
    }
    if (count == 0 || (count < 0 && errno == EIO)) {
        impl_->master.close();
        return {ReadState::Closed, 0};
    }
    fail("PTY read");
    return {ReadState::Closed, 0};
}
std::size_t PtyProcess::write(std::string_view bytes) {
    if (closed() || bytes.empty()) {
        return 0;
    }
    ssize_t count;
    do {
        count = ::write(impl_->master.value, bytes.data(), bytes.size());
    } while (count < 0 && errno == EINTR);
    if (count >= 0) {
        return std::size_t(count);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    // Do not close here: there may still be buffered output to drain. The read
    // side decides when EOF has been reached after a failed write.
    if (errno == EIO || errno == EPIPE) {
        return 0;
    }
    fail("PTY write");
    return 0;
}
void PtyProcess::resize(int columns, int rows) {
    dimensions(columns, rows);
    if (closed()) {
        return;
    }
    winsize size{};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    if (::ioctl(impl_->master.value, TIOCSWINSZ, &size) < 0 && errno != EIO &&
        errno != ENXIO) {
        fail("TIOCSWINSZ");
    }
}
std::optional<PtyProcess::ExitStatus> PtyProcess::pollExit() {
    return impl_->reap();
}
void PtyProcess::terminate() noexcept {
    impl_->terminate();
}
int PtyProcess::nativeHandle() const noexcept {
    return impl_->master.value;
}
bool PtyProcess::closed() const noexcept {
    return impl_->master.value < 0;
}
