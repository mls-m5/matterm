#pragma once

// The daemon of another computer through an ssh tunnel (--daemon-host,
// docs/REMOTE_DAEMON.md): shared by the window and by --attach-tty so that both
// ask and report the same way.
#include "daemonchoice.h"
#include "ptyfrontend.h"
#include "sshtunnel.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

// A yes/no question on the terminal this program runs in; no (without asking)
// when there is none.
inline bool askOnTerminal(const std::string &question) {
    if (!::isatty(0) || !::isatty(2)) {
        return false;
    }
    std::cerr << "matterm: " << question << " [y/N] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return answer == "y" || answer == "Y" || answer == "yes";
}

// Opens the tunnel for `options.host` and points `options.socketPath` at its
// end. Progress and questions go to stderr and stdin. Throws std::runtime_error
// with the reason.
inline std::unique_ptr<frontend::SshTunnel> openDaemonTunnel(
    DaemonOptions &options) {
    std::cerr << "matterm: connecting to " << options.host << " ...\n";
    frontend::SshTunnelOptions tunnelOptions;
    tunnelOptions.host = options.host;
    tunnelOptions.sshArguments = options.sshArguments;
    tunnelOptions.install = options.installAlways
                                ? frontend::DaemonInstall::Always
                                : frontend::DaemonInstall::Auto;
    tunnelOptions.assumeYes = options.assumeYes;
    tunnelOptions.progress = [](const std::string &line) {
        std::cerr << "matterm: " << line << "\n";
    };
    tunnelOptions.confirm = [](const std::string &question) {
        if (!::isatty(0) || !::isatty(2)) {
            return false; // Nobody to ask: --yes says yes in advance.
        }
        std::cerr << "matterm: " << question << " [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        return answer == "y" || answer == "Y" || answer == "yes";
    };
    std::string error;
    auto tunnel = frontend::SshTunnel::open(tunnelOptions, error);
    if (!tunnel) {
        throw std::runtime_error("--daemon-host: " + error);
    }
    options.socketPath = tunnel->socketPath();
    options.enabled = true;
    options.required = true;
    return tunnel;
}

// Which daemon to use, once its version is known (src/frontend/daemonchoice.h):
// the one on the socket if it is compatible, else the offer of a new one next
// to it. Sets `options.socketPath` to the socket to use and prints a note when
// one is worth it. Returns the reason when no daemon could be had.
// `startMissing`: start a daemon when none answers; `remote`: the daemon is on
// another computer, where nothing can be offered.
inline std::string selectDaemon(DaemonOptions &options,
                                bool remote,
                                bool startMissing) {
    frontend::DaemonChoiceTools tools;
    const auto own = mtd::ownVersion();
    tools.newSocket =
        mtd::versionedSocketPath(own.format, own.breaking, own.additive);
    tools.list = [] { return mtd::daemonSocketsIn(mtd::socketDirectory()); };
    tools.probe = [](const std::string &socket) {
        std::string error;
        return frontend::probeDaemon(socket, error);
    };
    // The default socket is the daemon's own default (its folder is made
    // private); any other path is given as it is.
    tools.start = [newSocket = tools.newSocket](const std::string &socket) {
        return frontend::startDaemon(socket == newSocket ? std::string()
                                                         : socket);
    };
    tools.ask = askOnTerminal;
    frontend::DaemonChoiceRequest request;
    request.socket = options.socketPath;
    request.offerNew = !remote;
    request.startMissing = startMissing;
    const auto choice = frontend::chooseDaemon(request, tools);
    if (!choice.error.empty()) {
        return choice.error;
    }
    if (!choice.note.empty()) {
        std::cerr << "matterm: " << choice.note << "\n";
    }
    options.socketPath = choice.socket;
    return {};
}
