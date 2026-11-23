#include "attachtty.h"

#include "clientsession.h"
#include "control.h"
#include "daemonclient.h"
#include "daemonconnect.h"
#include "daemonlink.h"
#include "ttyattach.h"
#include "ttyplain.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
using namespace frontend;
using namespace frontend::tty;

volatile std::sig_atomic_t gWinch = 0;
volatile std::sig_atomic_t gStop = 0;

void onWinch(int) {
    gWinch = 1;
}
void onStop(int) {
    gStop = 1;
}

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

protocol::Time daemonClock() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}

// Writes everything, also when the terminal takes it in pieces. False when it
// is gone.
bool writeAll(int fd, const std::string &bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n > 0) {
            done += std::size_t(n);
        }
        else if (n < 0 && errno == EINTR) {
            continue;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            ::poll(&p, 1, 1000);
        }
        else {
            return false;
        }
    }
    return true;
}

// The terminal this runs in: raw mode, the alternate screen, and everything put
// back on the way out, also when the mode throws or a signal asks us to stop.
class PlainHost final : public HostScreen {
public:
    PlainHost() {
        if (!::isatty(0) || !::isatty(1)) {
            throw std::runtime_error("--attach-tty needs a terminal: run it in "
                                     "a terminal (ssh -t from a phone)");
        }
        if (::tcgetattr(0, &saved_) != 0) {
            throw std::runtime_error(
                "--attach-tty: cannot read the terminal settings");
        }
        termios raw = saved_;
        ::cfmakeraw(&raw);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(0, TCSAFLUSH, &raw) != 0) {
            throw std::runtime_error(
                "--attach-tty: cannot put the terminal in raw mode");
        }
        active_ = true;
        const char *colorterm = std::getenv("COLORTERM");
        const std::string ct = colorterm ? colorterm : "";
        painter_.setCaps(PlainCaps{ct == "truecolor" || ct == "24bit"});
        // Alternate screen, bracketed paste on.
        write("\x1b[?1049h\x1b[?2004h\x1b[2J");
    }
    ~PlainHost() override {
        restore();
    }
    PlainHost(const PlainHost &) = delete;
    PlainHost &operator=(const PlainHost &) = delete;

    void restore() {
        if (!active_) {
            return;
        }
        active_ = false;
        write(mouseOff() + "\x1b[0m\x1b[?25h\x1b[?2004l\x1b[?1049l");
        ::tcsetattr(0, TCSAFLUSH, &saved_);
    }

    std::pair<int, int> size() const override {
        winsize w{};
        if (::ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
            return {int(w.ws_col), int(w.ws_row)};
        }
        return {80, 24};
    }
    void present(const ScreenImage &image) override {
        write(painter_.paint(image));
    }
    void setInputModes(const HostInputModes &modes) override {
        std::string out = mouseOff();
        if (modes.mouse) {
            out += "\x1b[?1000h";
            if (modes.mouseMotion) {
                out += "\x1b[?1002h";
            }
            if (modes.mouseAnyMotion) {
                out += "\x1b[?1003h";
            }
            out += "\x1b[?1006h";
        }
        write(out);
    }
    // The size changed: what is on the screen is not what was painted.
    void resized() {
        painter_.invalidate();
    }
    bool alive() const {
        return alive_;
    }

private:
    static std::string mouseOff() {
        return "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l";
    }
    void write(const std::string &bytes) {
        if (!bytes.empty() && !writeAll(1, bytes)) {
            alive_ =
                false; // The terminal went away (ssh dropped): the loop ends.
        }
    }

    termios saved_{};
    bool active_ = false;
    bool alive_ = true;
    PlainPainter painter_;
};

// A terminal of the daemon, taken over through the same client path as the
// window's panes.
class DaemonPane final : public AttachedPane {
public:
    DaemonPane(std::string socket, std::string id, int columns, int rows)
        : socket_(std::move(socket))
        , id_(std::move(id))
        , view_(config(columns, rows)) {
        link_ = attachToDaemon(view_, socket_, id_, 0, daemonClock);
    }
    ~DaemonPane() override {
        view_.detach(); // Before the link goes: the terminal itself keeps
                        // running in the daemon.
    }
    bool connected() const {
        return link_ != nullptr;
    }
    TerminalView &view() override {
        return view_;
    }
    void tick() override {
        if (link_) {
            link_->tick();
        }
    }
    bool reattach() override {
        if (reattachKind(view_) == Reattach::None) {
            return false;
        }
        // Always the same terminal: a missing one is the user's decision, not a
        // new shell.
        const std::string id = link_ ? link_->knownId() : id_;
        const std::uint64_t offset =
            link_ && !id.empty() ? link_->receivedOffset() : 0;
        auto fresh = attachToDaemon(view_, socket_, id, offset, daemonClock);
        link_ = std::move(fresh);
        return link_ != nullptr;
    }
    int pollFd() const override {
        return link_ ? link_->pollFd() : -1;
    }
    bool wantsWrite() const override {
        return link_ && link_->wantsWrite();
    }
    std::optional<std::int64_t> nextTickMs() const override {
        if (!link_) {
            return std::nullopt;
        }
        if (const auto due = link_->nextDeadline()) {
            return due->count();
        }
        return std::nullopt;
    }

private:
    static TerminalViewConfig config(int columns, int rows) {
        TerminalViewConfig c{std::size_t(columns), std::size_t(rows), 1000, {}};
        c.replacedNotice = "taken over by another client";
        return c;
    }
    std::string socket_;
    std::string id_;
    std::unique_ptr<DaemonLink>
        link_; // Declared before the view: the view lets go of it first.
    TerminalView view_;
};

class DaemonSource final : public SessionSource {
public:
    explicit DaemonSource(std::string socket)
        : socket_(std::move(socket)) {}

    std::vector<ListEntry> list(std::string &error) override {
        std::vector<ListEntry> entries;
        std::string why;
        const auto reply = askDaemon(socket_, mtd::ListSessions{}, why);
        const auto *listed =
            reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr;
        if (!listed) {
            error = daemonProblem(why);
            return entries;
        }
        // What runs there needs one more question per terminal; a few hundred
        // ms in all at most.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        bool asking = true;
        for (const mtd::SessionEntry &entry : listed->sessions) {
            std::optional<mtd::SessionInfo> info;
            if (asking && std::chrono::steady_clock::now() < deadline) {
                std::string ignored;
                const auto answer = askDaemon(
                    socket_, mtd::GetSessionInfo{entry.id}, ignored, 500);
                if (const auto *got =
                        answer ? std::get_if<mtd::SessionInfo>(&*answer)
                               : nullptr) {
                    info = *got;
                }
                else {
                    asking = false; // The daemon is slow or gone: show what the
                                    // list already says.
                }
            }
            entries.push_back(makeListEntry(entry, info ? &*info : nullptr));
        }
        return entries;
    }

    std::unique_ptr<AttachedPane> open(const std::string &id,
                                       int columns,
                                       int rows,
                                       std::string &error) override {
        auto pane = std::make_unique<DaemonPane>(socket_, id, columns, rows);
        if (!pane->connected()) {
            error = "Cannot reach the daemon";
            return nullptr;
        }
        return pane;
    }

private:
    std::string daemonProblem(const std::string &why) const {
        if (why.find("No such file") != std::string::npos ||
            why.find("refused") != std::string::npos) {
            return "The daemon is not running (" + socket_ + ")";
        }
        return "Cannot ask the daemon: " + why;
    }
    std::string socket_;
};

void installSignals() {
    struct sigaction winch {};
    winch.sa_handler = onWinch; // No SA_RESTART: poll(2) wakes up and the loop
                                // looks at the size.
    ::sigemptyset(&winch.sa_mask);
    ::sigaction(SIGWINCH, &winch, nullptr);
    struct sigaction stop {};
    stop.sa_handler = onStop;
    ::sigemptyset(&stop.sa_mask);
    ::sigaction(SIGTERM, &stop, nullptr);
    ::sigaction(SIGHUP, &stop, nullptr);
    ::sigaction(SIGINT, &stop, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
}

} // namespace

int runAttachTty(const DaemonOptions &daemonIn) {
    DaemonOptions daemon = daemonIn;
    // The tunnel comes before the terminal is touched: it may ask a question on
    // this terminal.
    std::unique_ptr<frontend::SshTunnel> tunnel;
    if (!daemon.host.empty()) {
        tunnel = openDaemonTunnel(daemon);
    }
    DaemonSource source(daemonSocket(daemon.socketPath));
    installSignals();
    PlainHost host;
    AttachTty mode(host, source);
    mode.start(steadyMs());
    std::int64_t lastInput = steadyMs();
    std::pair<int, int> size = host.size();
    while (!mode.finished() && !gStop && host.alive()) {
        std::int64_t now = steadyMs();
        const auto current = host.size();
        if (gWinch || current != size) {
            gWinch = 0;
            size = current;
            host.resized();
            mode.resized(now);
        }
        pollfd fds[2];
        nfds_t count = 0;
        fds[count++] = {0, POLLIN, 0};
        const int paneFd = mode.pollFd();
        if (paneFd >= 0) {
            fds[count++] = {
                paneFd,
                short(POLLIN | (mode.paneWantsWrite() ? POLLOUT : 0)),
                0};
        }
        const int rc = ::poll(fds, count, mode.timeoutMs(now));
        now = steadyMs();
        if (rc < 0 && errno != EINTR) {
            break;
        }
        if (rc > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[4096];
            const ssize_t n = ::read(0, buffer, sizeof buffer);
            if (n > 0) {
                mode.input(std::string_view(buffer, std::size_t(n)), now);
                lastInput = now;
            }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                break; // The terminal is gone (ssh dropped).
            }
        }
        else if (now - lastInput >= 30) {
            mode.quiet(now);
        }
        mode.tick(now);
    }
    host.restore();
    return 0;
}
