// matterm-attach: matterm --attach-tty without the window libraries (docs/tasks/daemon-versioning.md).
// One file that can be copied to another computer and run over ssh (`ssh -t host matterm-attach`):
// lists the terminals of the daemon, takes one over, and also lists and stops daemons.
#include "attachtty.h"
#include "daemoncli.h"
#include "version.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {
void usage(std::ostream &out) {
    out << "usage: matterm-attach [--daemon-socket PATH] [--daemon-host [USER@]HOST [--daemon-install] [--yes]\n"
           "                      [--ssh-arg ARG]...] [--daemon-list | --daemon-stop | --version]\n"
           "  Without a command: list the terminals that run in the daemon, take one over in this\n"
           "  terminal (full screen), leave it with Ctrl+].\n"
           "  --daemon-socket PATH  the daemon on this socket (default: the compatible daemon in\n"
           "                        $XDG_RUNTIME_DIR/matterm)\n"
           "  --daemon-host HOST    the daemon of another computer, through an ssh tunnel\n"
           "  --daemon-install      with --daemon-host: use our own matterm-daemon there\n"
           "  --yes                 copy without asking\n"
           "  --ssh-arg ARG         an extra argument for ssh (repeatable)\n"
           "  --daemon-list         list the daemons of this user (socket, pid, version, terminals)\n"
           "  --daemon-stop         stop a daemon after asking\n"
           "  --version             print the control version this program speaks\n";
}
} // namespace

int main(int argc, char **argv) {
    DaemonOptions options;
    options.enabled = true;
    bool list = false, stop = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--attach-tty") {
            continue; // Same as without: so that the same command line drives matterm and this.
        }
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "matterm-attach " << mtd::toString(mtd::ownVersion()) << "\n";
            return 0;
        }
        if (arg == "--daemon-list") {
            list = true;
        }
        else if (arg == "--daemon-stop") {
            stop = true;
        }
        else if (arg == "--daemon-install") {
            options.installAlways = true;
        }
        else if (arg == "--yes") {
            options.assumeYes = true;
        }
        else if (arg == "--daemon-socket" && i + 1 < argc) {
            options.socketPath = argv[++i];
        }
        else if (arg == "--daemon-host" && i + 1 < argc) {
            options.host = argv[++i];
        }
        else if (arg == "--ssh-arg" && i + 1 < argc) {
            options.sshArguments.emplace_back(argv[++i]);
        }
        else {
            usage(std::cerr);
            return 2;
        }
    }
    if (list && stop) {
        std::cerr << "--daemon-list and --daemon-stop cannot be combined\n";
        return 2;
    }
    if ((options.installAlways || options.assumeYes) && options.host.empty()) {
        std::cerr << "--daemon-install and --yes are for --daemon-host\n";
        return 2;
    }
    try {
        if (list) {
            return runDaemonList(options);
        }
        if (stop) {
            return runDaemonStop(options);
        }
        return runAttachTty(options);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
