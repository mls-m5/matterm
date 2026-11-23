#include "daemoncli.h"

#include "daemonadmin.h"

#include "daemonconnect.h"

#include <iostream>

namespace {
std::vector<std::string> socketsOf(const DaemonOptions &daemon) {
    if (!daemon.socketPath.empty()) {
        return {daemon.socketPath};
    }
    return frontend::findDaemonSockets(frontend::daemonSocketDirs());
}
} // namespace

namespace {
// The daemon list or stop of another computer: matmux there does it
// (copied first with
// --daemon-install), with this terminal.
int viaRemoteAttach(const DaemonOptions &daemon,
                    std::vector<std::string> args) {
    frontend::SshTunnelOptions options;
    options.host = daemon.host;
    options.sshArguments = daemon.sshArguments;
    options.install = daemon.installAlways ? frontend::DaemonInstall::Always
                                           : frontend::DaemonInstall::Auto;
    options.assumeYes = daemon.assumeYes;
    options.progress = [](const std::string &line) {
        std::cerr << "matterm: " << line << "\n";
    };
    options.confirm = askOnTerminal;
    if (!daemon.socketPath.empty()) { // A socket of the other computer.
        args.push_back("--daemon-socket");
        args.push_back(daemon.socketPath);
    }
    std::string error;
    const int code = frontend::SshTunnel::runRemoteAttach(options, args, error);
    if (code < 0) {
        std::cerr << "matterm: " << error << "\n";
        return 1;
    }
    return code;
}
} // namespace

int runDaemonList(const DaemonOptions &daemon) {
    if (!daemon.host.empty()) {
        return viaRemoteAttach(daemon, {"--daemon-list"});
    }
    const auto entries =
        frontend::listDaemons(socketsOf(daemon), frontend::realProbe());
    std::cout << frontend::formatDaemonList(entries);
    return 0;
}

int runDaemonStop(const DaemonOptions &daemon) {
    if (!daemon.host.empty()) {
        return viaRemoteAttach(daemon, {"--daemon-stop"});
    }
    const auto sockets = socketsOf(daemon);
    if (sockets.empty()) {
        std::cout << "No daemon found.\n";
        return 0;
    }
    if (sockets.size() > 1) {
        std::cout << frontend::formatDaemonList(
                         frontend::listDaemons(sockets, frontend::realProbe()))
                  << "Several daemons run: name the one to stop with "
                     "--daemon-socket SOCKET.\n";
        return 1;
    }
    const auto result = frontend::stopDaemon(
        sockets.front(), frontend::realStopTools(askOnTerminal));
    std::cout << result.message << "\n";
    return result.stopped ? 0 : 1;
}
