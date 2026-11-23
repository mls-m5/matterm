#include "ptyprocess.h"

#include <algorithm>
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
#include <sys/syscall.h>
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

std::vector<std::string> environment(
    const std::string &term,
    PtyProcess::ColorCapability color,
    const std::optional<std::vector<std::string>> &allowList,
    const std::vector<std::string> &extra) {
    std::vector<std::string> result;
    for (char **entry = environ; entry && *entry; ++entry) {
        const std::string value = *entry;
        if (value.compare(0, 5, "TERM=") == 0 ||
            value.compare(0, 10, "COLORTERM=") == 0 ||
            value.compare(0, 8, "COLUMNS=") == 0 ||
            value.compare(0, 6, "LINES=") == 0) {
            continue;
        }
        const auto name = value.substr(0, value.find('='));
        if (!allowList ||
            std::find(allowList->begin(), allowList->end(), name) !=
                allowList->end()) {
            result.push_back(value);
        }
    }
    for (const auto &entry : extra) {
        const auto name = entry.substr(0, entry.find('='));
        result.erase(std::remove_if(result.begin(),
                                    result.end(),
                                    [&](const std::string &e) {
                                        return e.compare(0,
                                                         name.size() + 1,
                                                         name + "=") == 0;
                                    }),
                     result.end());
        result.push_back(entry);
    }
    result.push_back("TERM=" + term);
    if (color == PtyProcess::ColorCapability::TrueColor) {
        result.emplace_back("COLORTERM=truecolor");
    }
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
               const std::string &term,
               ColorCapability color,
               const std::optional<std::vector<std::string>> &allowList,
               const std::string &workingDirectory,
               const std::vector<std::string> &extraEnvironment) {
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
        if (workingDirectory.find('\0') != std::string::npos) {
            throw std::invalid_argument("NUL in working directory");
        }
        const auto path = executable(arguments[0]);
        for (const auto &entry : extraEnvironment) {
            if (entry.find('=') == std::string::npos ||
                entry.find('\0') != std::string::npos) {
                throw std::invalid_argument("bad extra environment entry");
            }
        }
        auto env = environment(term, color, allowList, extraEnvironment);
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
        const long maxOpen = ::sysconf(_SC_OPEN_MAX);
        if (maxOpen < 0)
            fail("sysconf open maximum");
        child = ::forkpty(&master.value, nullptr, nullptr, &size);
        if (child < 0) {
            fail("forkpty");
        }
        if (child == 0) {
            ::close(errorRead.value);
            // In particular, never leak listener/client sockets or credentials
            // into the shell. Preserve only stdio and the CLOEXEC error pipe.
            bool rangesClosed = false;
#ifdef SYS_close_range
            const auto lower =
                errorWrite.value <= 3
                    ? 0
                    : ::syscall(SYS_close_range,
                                3u,
                                static_cast<unsigned>(errorWrite.value - 1),
                                0u);
            const auto upper =
                ::syscall(SYS_close_range,
                          static_cast<unsigned>(errorWrite.value + 1),
                          UINT_MAX,
                          0u);
            rangesClosed = lower == 0 && upper == 0;
#endif
            if (!rangesClosed) {
                for (long fd = 3; fd < maxOpen; ++fd) {
                    if (fd != errorWrite.value)
                        ::close(static_cast<int>(fd));
                }
            }
            // A folder that cannot be entered is not an error: stay where we are.
            if (!workingDirectory.empty() && ::chdir(workingDirectory.c_str()) != 0) {
                // Deliberately ignored.
            }
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

PtyProcess::PtyProcess(
    const std::vector<std::string> &arguments,
    int columns,
    int rows,
    const std::string &term,
    ColorCapability color,
    std::optional<std::vector<std::string>> environmentAllowList,
    const std::string &workingDirectory,
    const std::vector<std::string> &extraEnvironment)
    : impl_(std::make_unique<Impl>()) {
    impl_->start(arguments,
                 columns,
                 rows,
                 term,
                 color,
                 environmentAllowList,
                 workingDirectory,
                 extraEnvironment);
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
void PtyProcess::resize(int columns,
                        int rows,
                        int pixelWidth,
                        int pixelHeight) {
    dimensions(columns, rows);
    if (pixelWidth < 0 || pixelWidth > USHRT_MAX || pixelHeight < 0 ||
        pixelHeight > USHRT_MAX) {
        throw std::invalid_argument("PTY pixels must be in 0..65535");
    }
    if (closed()) {
        return;
    }
    winsize size{};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_xpixel = static_cast<unsigned short>(pixelWidth);
    size.ws_ypixel = static_cast<unsigned short>(pixelHeight);
    if (::ioctl(impl_->master.value, TIOCSWINSZ, &size) < 0 && errno != EIO &&
        errno != ENXIO) {
        fail("TIOCSWINSZ");
    }
}
std::optional<PtyProcess::ExitStatus> PtyProcess::peekExit() {
    if (impl_->status)
        return impl_->status;
    siginfo_t info{};
    if (::waitid(P_PID, impl_->child, &info, WEXITED | WNOHANG | WNOWAIT) ==
            0 &&
        info.si_pid == impl_->child) {
        return ExitStatus{info.si_code == CLD_EXITED ? info.si_status : -1,
                          info.si_code == CLD_EXITED ? 0 : info.si_status};
    }
    return std::nullopt;
}
std::optional<PtyProcess::ExitStatus> PtyProcess::pollExit() {
    return impl_->reap();
}
void PtyProcess::signalProcessGroup(int signal) noexcept {
    if (impl_->child > 0 && !impl_->status) {
        ::kill(-impl_->child, signal);
    }
}
void PtyProcess::terminate() noexcept {
    impl_->terminate();
}
int PtyProcess::nativeHandle() const noexcept {
    return impl_->master.value;
}
int PtyProcess::childPid() const noexcept {
    return impl_->child > 0 && !impl_->status ? int(impl_->child) : 0;
}
int PtyProcess::foregroundProcessGroup() const noexcept {
    if (impl_->master.value < 0) return 0;
    const pid_t group = ::tcgetpgrp(impl_->master.value);
    return group > 0 ? int(group) : 0;
}
bool PtyProcess::closed() const noexcept {
    return impl_->master.value < 0;
}
