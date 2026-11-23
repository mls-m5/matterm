// matmux's default way to show a terminal (docs/tasks/matmux-passthrough.md):
// the daemon's output goes to the host terminal unchanged, so the host keeps
// its scrollback, selection and mouse. This file is the I/O around
// frontend::passthrough::Session: the raw mode and the way back on every exit
// (also a signal or a crash), the poll loop with the limit on what waits for
// the host, and the round of list, terminal and lines in between.
#include "attachtty.h"
#include "attachtty_impl.h"
#include "passthrough.h"
#include "tty/posix/terminal.h"

#include "tty/matterm/snapshot.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
using namespace attachimpl;
using frontend::passthrough::Outcome;

// ---- what the host terminal is put back to
// --------------------------------------------------------

std::string snapshotOf(const frontend::TerminalView &view) {
    return tty::snapshot(view.model(), view.modes());
}

std::string restoreFor(bool inAlternateScreen) {
    return tty::hostRestore(inAlternateScreen);
}

// ---- the host terminal in raw mode, put back on every way out
// -------------------------------------

int gWake[2] = {-1, -1}; // Signals say what happened by writing a byte here.
char gRestoreBuf[2048];  // What a crash handler writes (kept up to date by the
                         // loop).
volatile std::size_t gRestoreLen = 0;
termios gSaved{};
volatile std::sig_atomic_t gRaw = 0;

void onSignal(int sig) {
    const char byte = char(sig);
    const int saved = errno;
    (void)!::write(gWake[1], &byte, 1);
    errno = saved;
}

void onCrash(int sig) {
    if (gRaw) {
        (void)!::write(1, gRestoreBuf, gRestoreLen);
        (void)::tcsetattr(0, TCSANOW, &gSaved);
    }
    (void)::signal(sig, SIG_DFL);
    (void)::raise(sig);
}

void setCrashRestore(const std::string &bytes) {
    const std::size_t n = std::min(bytes.size(), sizeof gRestoreBuf);
    gRestoreLen = 0;
    std::copy_n(bytes.data(), n, gRestoreBuf); // Raw bytes, not a C string.
    gRestoreLen = n;
}

class RawHost {
public:
    RawHost() {
        if (!::isatty(0) || !::isatty(1) || ::tcgetattr(0, &gSaved) != 0) {
            throw std::runtime_error("This needs a terminal: run it in a "
                                     "terminal (ssh -t from a phone)");
        }
        if (::pipe(gWake) != 0) {
            throw std::runtime_error("cannot make a pipe");
        }
        for (const int fd : gWake) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        setCrashRestore(tty::hostRestore(false));
        termios raw = gSaved;
        ::cfmakeraw(&raw);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        gRaw = 1;
        if (::tcsetattr(0, TCSADRAIN, &raw) != 0) {
            gRaw = 0;
            throw std::runtime_error("cannot put the terminal in raw mode");
        }
        struct sigaction wake {};
        wake.sa_handler = onSignal;
        ::sigemptyset(&wake.sa_mask);
        for (const int sig : {SIGWINCH, SIGTERM, SIGHUP, SIGINT}) {
            ::sigaction(sig, &wake, &old_[sig]);
        }
        struct sigaction crash {};
        crash.sa_handler = onCrash;
        ::sigemptyset(&crash.sa_mask);
        crash.sa_flags = SA_NODEFER;
        for (const int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) {
            ::sigaction(sig, &crash, &old_[sig]);
        }
        (void)::signal(SIGPIPE, SIG_IGN);
        active_ = true;
    }
    ~RawHost() {
        restore("");
    }
    RawHost(const RawHost &) = delete;
    RawHost &operator=(const RawHost &) = delete;

    // Writes `bytes` (what puts the terminal back), then the settings; safe to
    // call twice.
    void restore(const std::string &bytes) {
        if (!active_) {
            return;
        }
        active_ = false;
        writeAll(bytes);
        gRaw = 0;
        ::tcsetattr(0, TCSAFLUSH, &gSaved);
        for (const int sig : {SIGWINCH,
                              SIGTERM,
                              SIGHUP,
                              SIGINT,
                              SIGSEGV,
                              SIGBUS,
                              SIGABRT,
                              SIGFPE,
                              SIGILL}) {
            ::sigaction(sig, &old_[sig], nullptr);
        }
        for (int &fd : gWake) {
            ::close(fd);
            fd = -1;
        }
    }
    static void size(int &columns, int &rows) {
        winsize w{};
        columns = 80;
        rows = 24;
        if (::ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
            columns = w.ws_col;
            rows = w.ws_row;
        }
    }
    static bool writeAll(const std::string &bytes) {
        return ::tty::writeAll(1, bytes);
    }

private:
    struct sigaction old_[65]{};
    bool active_ = false;
};

// One terminal shown by passthrough until it ends in some way. `title` names it
// in the lines.
Outcome showTerminal(DaemonPane &pane,
                     const std::string &title,
                     const std::string &attachLine) {
    RawHost host;
    int columns = 0, rows = 0;
    RawHost::size(columns, rows);
    frontend::passthrough::Session::Options options;
    options.title = title;
    options.attachLine = attachLine;
    options.snapshot = snapshotOf;
    options.restore = restoreFor;
    frontend::passthrough::Session session(pane, options, steadyMs());
    bool interrupted = false;
    bool hostGone = false;
    bool alt = false;
    while (!hostGone) {
        const std::int64_t now = steadyMs();
        const auto &out = session.hostOut();
        if (session.outcome() && out.empty()) {
            break; // Its last words are written.
        }
        pollfd fds[4];
        nfds_t count = 0;
        fds[count++] = {0, POLLIN, 0};
        fds[count++] = {gWake[0], POLLIN, 0};
        const int paneFd = session.wantsRead() ? pane.pollFd() : -1;
        if (paneFd >= 0) {
            fds[count++] = {
                paneFd, short(POLLIN | (pane.wantsWrite() ? POLLOUT : 0)), 0};
        }
        const nfds_t outIndex = count;
        if (!out.empty()) {
            fds[count++] = {1, POLLOUT, 0};
        }
        int timeout =
            session.live()
                ? -1
                : 50; // Until the screen is shown the replay is waited for.
        if (const auto due = pane.nextTickMs()) {
            const int left = int(std::max<std::int64_t>(0, *due - now));
            timeout = timeout < 0 ? left : std::min(timeout, left);
        }
        if (session.outcome()) {
            timeout = -1; // Only the writing is left.
        }
        const int rc = ::poll(fds, count, timeout);
        if (rc < 0 && errno != EINTR) {
            break;
        }
        if (rc > 0) {
            if (fds[1].revents & POLLIN) {
                char signals[16];
                const ssize_t n = ::read(gWake[0], signals, sizeof signals);
                for (ssize_t i = 0; i < n; ++i) {
                    if (signals[i] == SIGWINCH) {
                        RawHost::size(columns, rows);
                        session.resized(columns, rows);
                    }
                    else {
                        interrupted = true;
                    }
                }
            }
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) &&
                !session.outcome()) {
                char buffer[4096];
                const ssize_t n = ::read(0, buffer, sizeof buffer);
                if (n > 0) {
                    session.input(std::string_view(buffer, std::size_t(n)));
                }
                else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    interrupted = true; // The terminal is gone (ssh dropped).
                }
            }
            if (outIndex < count &&
                (fds[outIndex].revents & (POLLOUT | POLLERR | POLLHUP))) {
                const std::size_t chunk =
                    std::min<std::size_t>(session.hostOut().size(), 65536);
                const ssize_t n = ::write(1, session.hostOut().data(), chunk);
                if (n > 0) {
                    session.consumed(std::size_t(n));
                }
                else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                    hostGone = true;
                }
            }
        }
        session.tick(steadyMs());
        if (session.live() && pane.view().model().alternateScreen() != alt) {
            alt = !alt;
            setCrashRestore(session.restore());
        }
        if (interrupted) {
            break;
        }
    }
    host.restore(session.restore());
    if (session.outcome()) {
        return *session.outcome();
    }
    return Outcome{Outcome::Kind::Leave, 0, ""};
}

// Plain lines on the restored terminal.
void say(const std::string &line) {
    std::cout << "matmux: " << line << std::endl;
}

} // namespace

int runPassthroughMode(const DaemonOptions &daemon,
                       attachimpl::DaemonSource &source,
                       const std::string &own) {
    enum class Next { List, New, Attach };
    Next next = daemon.attachNew ? Next::New : Next::List;
    bool firstNew =
        daemon.attachNew; // The client ends with the program, as with --new.
    frontend::ttymode::NewTerminal request;
    request.name = daemon.attachName;
    request.command = daemon.attachCommand;
    std::string id, title, message;
    bool hinted = false;
    for (;;) {
        if (next == Next::List) {
            TtyHost host;
            AttachTty::Options options;
            options.ownSession = own;
            options.comeBack = daemon.attachProgram;
            options.listOnly = true;
            options.selectNew = daemon.attachDefault; // matmux without arguments: Enter = a new terminal.
            options.initialMessage = message;
            AttachTty list(host, source, options);
            list.start(steadyMs());
            std::int64_t lastInput = steadyMs();
            bool running = true;
            while (running && !list.finished() && host.alive()) {
                pollfd fds[2] = {{0, POLLIN, 0},
                                 {host.session().wakeFd(), POLLIN, 0}};
                const int rc = ::poll(fds, 2, list.timeoutMs(steadyMs()));
                const std::int64_t now = steadyMs();
                if (rc < 0 && errno != EINTR) {
                    break;
                }
                const auto events = host.session().takeEvents();
                if (events.quit) {
                    break;
                }
                if (events.resized) {
                    list.resized(now);
                }
                if (rc > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                    char buffer[4096];
                    const ssize_t n = ::read(0, buffer, sizeof buffer);
                    if (n > 0) {
                        list.input(std::string_view(buffer, std::size_t(n)),
                                   now);
                        lastInput = now;
                    }
                    else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                        running = false;
                    }
                }
                else if (now - lastInput >= 30) {
                    list.quiet(now);
                }
                list.tick(now);
            }
            host.restore(); // Leaves the alternate screen: the host's
                            // scrollback is what it was.
            const auto choice = list.choice();
            message.clear();
            if (choice.kind == AttachTty::Choice::Kind::None) {
                return 0;
            }
            if (choice.kind == AttachTty::Choice::Kind::New) {
                request = {};
                next = Next::New;
            }
            else {
                id = choice.id;
                title = choice.title;
                next = Next::Attach;
            }
        }
        if (next == Next::New) {
            int columns = 0, rows = 0;
            RawHost::size(columns, rows);
            std::string error;
            id = source.create(request, columns, rows, error);
            title = request.name.empty() ? "new terminal" : request.name;
            if (id.empty()) {
                if (firstNew) {
                    say(error.empty() ? "The daemon could not start a terminal"
                                      : error);
                    return 1;
                }
                message = error;
                next = Next::List;
                continue;
            }
            next = Next::Attach;
            request = {};
        }
        // next == Attach: show terminal `id`.
        int columns = 0, rows = 0;
        RawHost::size(columns, rows);
        const std::uint64_t end = source.outputEnd(id);
        DaemonPane pane(source.socket(), id, columns, rows, end);
        if (!pane.connected()) {
            if (firstNew) {
                say("Cannot reach the daemon");
                return 1;
            }
            message = "Cannot reach the daemon";
            next = Next::List;
            continue;
        }
        const std::string line =
            hinted ? std::string()
                   : std::string("matmux: Ctrl+B d leaves (the terminal keeps "
                                 "running), l the list, c a new terminal");
        hinted = true;
        const Outcome outcome = showTerminal(pane, title, line);
        switch (outcome.kind) {
        case Outcome::Kind::Leave:
            say(title + " keeps running; " + daemon.attachProgram +
                " comes back to it");
            return 0;
        case Outcome::Kind::List:
            next = Next::List;
            firstNew = false;
            break;
        case Outcome::Kind::New:
            next = Next::New;
            firstNew = false;
            break;
        case Outcome::Kind::Ended:
            say(outcome.message);
            if (firstNew) {
                return outcome.exitCode;
            }
            message = outcome.message;
            next = Next::List;
            break;
        case Outcome::Kind::Gone:
            say(outcome.message);
            message = outcome.message;
            next = Next::List;
            firstNew = false;
            break;
        case Outcome::Kind::TakenOver:
        case Outcome::Kind::Lost: {
            say(outcome.message +
                ". Enter takes it back, l the list, d leaves (then Enter)");
            std::string answer;
            if (!std::getline(std::cin, answer) || answer == "d") {
                say(title + " keeps running; " + daemon.attachProgram +
                    " comes back to it");
                return 0;
            }
            if (answer == "l") {
                next = Next::List;
                firstNew = false;
            }
            else {
                next = Next::Attach; // Same terminal again: a new attach, a new
                                     // snapshot.
            }
            break;
        }
        }
    }
}
