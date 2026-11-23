#include "attachtty.h"

#include "clientsession.h"
#include "control.h"
#include "daemonclient.h"
#include "daemonconnect.h"
#include "daemonlink.h"
#include "tty/posix/session.h"
#include "tty/posix/terminal.h"
#include "ttyattach.h"

#include <algorithm>
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
#include <unistd.h>

namespace {
using namespace frontend;
using namespace frontend::ttymode;

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

protocol::Time daemonClock() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}

// The terminal this runs in, through the shared tty layer (src/tty): raw mode,
// the alternate screen, the size, SIGWINCH and the way back on every exit. The
// mouse is asked for by this class itself, only while the program in the pane
// wants it (the layer's option is all or nothing).
class TtyHost final : public HostScreen {
public:
    TtyHost()
        : session_(sessionOptions()) {
        if (!session_.start()) {
            throw std::runtime_error("This needs a terminal: run it in "
                                     "a terminal (ssh -t from a phone)");
        }
        session_.screen().canvas().setWideChars(true);
    }
    ~TtyHost() override {
        restore();
    }
    TtyHost(const TtyHost &) = delete;
    TtyHost &operator=(const TtyHost &) = delete;

    void restore() {
        session_.stop();
    }
    ::tty::Screen &screen() override {
        return session_.screen();
    }
    void present() override {
        if (!session_.present()) {
            alive_ =
                false; // The terminal went away (ssh dropped): the loop ends.
        }
    }
    void setInputModes(const HostInputModes &modes) override {
        ::tty::ScreenOptions options;
        options.bracketedPaste = true;
        if (modes.mouse) {
            options.mouse =
                modes.mouseAnyMotion ? ::tty::ScreenOptions::Mouse::Any
                : modes.mouseMotion  ? ::tty::ScreenOptions::Mouse::Drag
                                     : ::tty::ScreenOptions::Mouse::Click;
        }
        // The layer undoes all of it on every way out, a crash included.
        if (!::tty::writeAll(1, session_.screen().setOptions(options))) {
            alive_ = false;
        }
    }
    ::tty::Session &session() {
        return session_;
    }
    bool alive() const {
        return alive_;
    }

private:
    static ::tty::SessionOptions sessionOptions() {
        ::tty::SessionOptions options;
        options.screen.bracketedPaste = true;
        return options;
    }
    ::tty::Session session_;
    bool alive_ = true;
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
    // `ownSession`: the terminal this program runs in (MATTERM_SESSION), marked
    // in the list. `cwd`: the folder a new terminal starts in (empty for a
    // daemon on another computer: its own folder).
    DaemonSource(std::string socket, std::string ownSession, std::string cwd)
        : socket_(std::move(socket))
        , own_(std::move(ownSession))
        , cwd_(std::move(cwd)) {}

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
            entries.push_back(
                makeListEntry(entry, info ? &*info : nullptr, own_));
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

    std::string create(const NewTerminal &what,
                       int columns,
                       int rows,
                       std::string &error) override {
        mtd::CreateTerminal request;
        request.cwd = cwd_;
        request.columns = std::uint16_t(std::clamp(columns, 1, 500));
        request.rows = std::uint16_t(std::clamp(rows, 1, 500));
        request.command = what.command;
        if (mtd::validTerminalName(what.name)) {
            request.name = what.name;
        }
        std::string why;
        const auto id = createTerminal(socket_, request, why);
        if (!id) {
            error = "The daemon could not start a terminal: " +
                    (why.empty() ? daemonProblem(why) : why);
            return {};
        }
        return *id;
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
    std::string own_;
    std::string cwd_;
};

} // namespace

int runAttachTty(const DaemonOptions &daemonIn) {
    DaemonOptions daemon = daemonIn;
    // The tunnel comes before the terminal is touched: it may ask a question on
    // this terminal.
    std::unique_ptr<frontend::SshTunnel> tunnel;
    if (!daemon.host.empty()) {
        tunnel = openDaemonTunnel(daemon);
    }
    // A daemon of an incompatible version is refused, or a new one offered next
    // to it.
    if (const auto error =
            selectDaemon(daemon, tunnel != nullptr, daemon.attachNew);
        !error.empty()) {
        std::cerr << "matterm: " << error << "\n";
        return 1;
    }
    const char *ownText = std::getenv(
        "MATTERM_SESSION"); // Set by the daemon in every terminal it starts.
    const std::string own = ownText ? ownText : "";
    std::string cwd;
    if (tunnel == nullptr) {
        char buffer[4096];
        if (::getcwd(buffer, sizeof buffer)) {
            cwd = buffer;
        }
    }
    DaemonSource source(daemonSocket(daemon.socketPath), own, cwd);
    (void)::signal(SIGPIPE, SIG_IGN); // A write to a gone terminal is an error
                                      // return, not a signal.
    TtyHost host;
    AttachTty::Options options;
    options.ownSession = own;
    options.comeBack = daemon.attachProgram;
    options.startNew = daemon.attachNew;
    options.exitWhenEnded = daemon.attachNew;
    options.newTerminal.name = daemon.attachName;
    options.newTerminal.command = daemon.attachCommand;
    AttachTty mode(host, source, options);
    mode.start(steadyMs());
    std::int64_t lastInput = steadyMs();
    bool running = true;
    while (running && !mode.finished() && host.alive()) {
        std::int64_t now = steadyMs();
        pollfd fds[3];
        nfds_t count = 0;
        fds[count++] = {0, POLLIN, 0};
        fds[count++] = {host.session().wakeFd(), POLLIN, 0};
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
        const auto events = host.session().takeEvents();
        if (events.quit) {
            break; // SIGTERM, SIGHUP, SIGINT: the layer puts the terminal back.
        }
        if (events.resized) {
            mode.resized(now);
        }
        if (rc > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[4096];
            const ssize_t n = ::read(0, buffer, sizeof buffer);
            if (n > 0) {
                mode.input(std::string_view(buffer, std::size_t(n)), now);
                lastInput = now;
            }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                running = false; // The terminal is gone (ssh dropped).
            }
        }
        else if (now - lastInput >= 30) {
            mode.quiet(now);
        }
        mode.tick(now);
    }
    host.restore();
    if (!mode.farewell().empty()) {
        std::cout << mode.farewell() << "\n"; // After the terminal is back.
    }
    return mode.exitCode();
}
