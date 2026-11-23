#pragma once

// The daemon of another computer through an ssh tunnel (--daemon-host,
// docs/REMOTE_DAEMON.md): shared by the window and by --attach-tty so that both
// ask and report the same way.
#include "ptyfrontend.h"
#include "sshtunnel.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

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
