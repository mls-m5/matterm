#include "tty/posix/session.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace tty {
namespace {

// Everything a signal handler needs, prepared before the handlers exist.
struct Shared {
    std::atomic<bool> active{false};
    int inputFd = -1;
    int outputFd = -1;
    int pipe[2] = {-1, -1};
    termios cooked{}; // The terminal as it was.
    termios raw{};    // The terminal while the session runs.
    std::string enterBytes;
    std::string restoreBytes;
    bool atexitRegistered = false;
};
Shared g;

void writeBytes(int fd, const std::string &bytes) {
    const char *data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const auto n = ::write(fd, data, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        data += n;
        left -= std::size_t(n);
    }
}

void notify(char what) {
    const int saved = errno;
    (void)!::write(g.pipe[1], &what, 1);
    errno = saved;
}

// The way back to the terminal as it was; async-signal-safe.
void restoreTerminal() {
    writeBytes(g.outputFd, g.restoreBytes);
    tcsetattr(g.inputFd, TCSADRAIN, &g.cooked);
}

void installOne(int signal, void (*handler)(int), int flags) {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = flags;
    sigaction(signal, &action, nullptr);
}

void onWinch(int) {
    notify('w');
}
void onQuit(int) {
    notify('q');
}
void onFatal(int signal) {
    if (g.active.exchange(false)) {
        restoreTerminal();
    }
    installOne(signal, SIG_DFL, 0);
    (void)raise(signal);
}
void onStop(int) {
    if (g.active.exchange(false)) {
        restoreTerminal();
    }
    installOne(SIGTSTP, SIG_DFL, 0);
    (void)raise(SIGTSTP); // Stops here; continues after SIGCONT.
}
void onContinue(int) {
    installOne(SIGTSTP, onStop, 0);
    if (!g.active.exchange(true)) {
        tcsetattr(g.inputFd, TCSADRAIN, &g.raw);
        writeBytes(g.outputFd, g.enterBytes);
    }
    notify('c');
}
void atExitRestore() {
    if (g.active.exchange(false)) {
        restoreTerminal();
    }
}

constexpr int kFatal[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr int kQuit[] = {SIGINT, SIGTERM, SIGHUP};

alignas(16) char gAltStack[1 << 16];
} // namespace

Session::Session(SessionOptions options)
    : options_(std::move(options)) {}

Session::~Session() {
    stop();
}

bool Session::start() {
    if (running_ || g.active.load() || g.pipe[0] != -1) {
        return false;
    }
    RawMode probe(options_.inputFd);
    if (!probe.enter()) {
        return false; // Not a terminal.
    }
    probe.restore();
    int columns = 80, rows = 24;
    windowSize(options_.outputFd, columns, rows);
    const Caps caps = options_.detect ? detectCaps() : options_.caps;
    screen_ = std::make_unique<Screen>(columns, rows, caps, options_.screen);

    if (pipe(g.pipe) != 0) {
        return false;
    }
    for (const int fd : g.pipe) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    g.inputFd = options_.inputFd;
    g.outputFd = options_.outputFd;
    tcgetattr(g.inputFd, &g.cooked);
    g.raw = g.cooked;
    cfmakeraw(&g.raw);
    g.raw.c_cc[VMIN] = 1;
    g.raw.c_cc[VTIME] = 0;
    g.enterBytes = screen_->enter();
    g.restoreBytes = screen_->restoreSequence();

    stack_t stack{};
    stack.ss_sp = gAltStack;
    stack.ss_size = sizeof gAltStack;
    sigaltstack(&stack, nullptr);

    tcsetattr(g.inputFd, TCSADRAIN, &g.raw);
    writeBytes(g.outputFd, g.enterBytes);
    g.active = true;
    installOne(SIGWINCH, onWinch, SA_RESTART);
    for (const int signal : kQuit) {
        installOne(signal, onQuit, 0);
    }
    for (const int signal : kFatal) {
        installOne(signal, onFatal, SA_ONSTACK);
    }
    installOne(SIGTSTP, onStop, 0);
    installOne(SIGCONT, onContinue, SA_RESTART);
    if (!g.atexitRegistered) {
        g.atexitRegistered = true;
        (void)std::atexit(atExitRestore);
    }
    running_ = true;
    return true;
}

void Session::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    for (const int signal : {SIGWINCH, SIGTSTP, SIGCONT}) {
        installOne(signal, SIG_DFL, 0);
    }
    for (const int signal : kQuit) {
        installOne(signal, SIG_DFL, 0);
    }
    for (const int signal : kFatal) {
        installOne(signal, SIG_DFL, 0);
    }
    if (g.active.exchange(false)) {
        restoreTerminal();
    }
    screen_->leave();
    for (int &fd : g.pipe) {
        close(fd);
        fd = -1;
    }
}

bool Session::present() {
    return writeAll(options_.outputFd, screen_->present());
}

int Session::wakeFd() const {
    return g.pipe[0];
}

Session::Events Session::takeEvents() {
    Events events;
    char buffer[64];
    for (;;) {
        const auto n = read(g.pipe[0], buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            switch (buffer[i]) {
            case 'w':
                events.resized = true;
                break;
            case 'q':
                events.quit = true;
                break;
            case 'c':
                events.resumed = true;
                break;
            default:
                break;
            }
        }
    }
    if (events.resized || events.resumed) {
        int columns = screen_->columns(), rows = screen_->rows();
        windowSize(options_.outputFd, columns, rows);
        if (columns != screen_->columns() || rows != screen_->rows()) {
            screen_->resize(columns, rows);
            events.resized = true;
        }
        if (events.resumed) {
            screen_->redraw();
        }
    }
    return events;
}

} // namespace tty
