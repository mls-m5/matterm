#include "attachtty.h"

#include "attachtty_impl.h"
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
using namespace attachimpl;
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
    if (!daemon.rendered) {
        return runPassthroughMode(daemon, source, own);
    }
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
