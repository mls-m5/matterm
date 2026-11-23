// matmux: matterm --attach-tty without the window libraries (docs/tasks/daemon-versioning.md).
// One file that can be copied to another computer and run over ssh (`ssh -t host matmux`):
// lists the terminals of the daemon, takes one over, and also lists and stops daemons.
#include "attachtty.h"
#include "daemoncli.h"
#include "daemonclient.h"
#include "socketpath.h"
#include "version.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {
void usage(std::ostream &out) {
    out << "usage: matmux [--daemon-socket PATH] [--daemon-host [USER@]HOST "
           "[--daemon-install] [--yes]\n"
           "                      [--ssh-arg ARG]...] [--daemon-list | "
           "--daemon-stop | --version]\n"
           "       matmux [--daemon-socket PATH] --new [--name NAME] [-- "
           "COMMAND...]\n"
           "       matmux [--daemon-socket PATH] --attach\n"
           "  Without arguments: a new terminal with your shell, started in "
           "the daemon (started when\n"
           "  missing), attached at once. Leave it with Ctrl+B d: it keeps "
           "running.\n"
           "  --attach              list the terminals that run in the daemon "
           "and take one over\n"
           "                        in this terminal (full screen; Ctrl+B l is "
           "the list again)\n"
           "  --daemon-socket PATH  the daemon on this socket (default: the "
           "compatible daemon in\n"
           "                        $XDG_RUNTIME_DIR/matterm)\n"
           "  --daemon-host HOST    the daemon of another computer, through an "
           "ssh tunnel\n"
           "  --daemon-install      with --daemon-host: use our own "
           "matterm-daemon there\n"
           "  --yes                 copy without asking\n"
           "  --ssh-arg ARG         an extra argument for ssh (repeatable)\n"
           "  --new                 start a terminal (your shell, or COMMAND) "
           "in the daemon, started when\n"
           "                        missing, and attach to it at once; the "
           "program ends the client\n"
           "  --name NAME           with --new: a name for the terminal\n"
           "  --daemon-list         list the daemons of this user (socket, "
           "pid, version, terminals)\n"
           "  --daemon-stop         stop a daemon after asking\n"
           "  --version             print the control version this program "
           "speaks\n";
}
} // namespace

int main(int argc, char **argv) {
    DaemonOptions options;
    options.enabled = true;
    options.attachProgram = "matmux --attach";
    bool list = false, stop = false, attach = false, implicitNew = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--attach") {
            attach = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "matmux " << mtd::toString(mtd::ownVersion()) << "\n";
            return 0;
        }
        if (arg == "--") {
            while (++i < argc) {
                options.attachCommand.emplace_back(argv[i]);
            }
            break;
        }
        if (arg == "--new") {
            options.attachNew = true;
        }
        else if (arg == "--name" && i + 1 < argc) {
            options.attachName = argv[++i];
        }
        else if (arg == "--daemon-list") {
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
    // A name or a command is for a new terminal; without any mode, a new
    // terminal is what matmux does.
    if (!attach && !list && !stop && !options.attachNew) {
        options.attachNew = true;
        implicitNew =
            options.attachName.empty() && options.attachCommand.empty();
    }
    if (attach && (!options.attachName.empty() ||
                   !options.attachCommand.empty() || options.attachNew)) {
        std::cerr << "--attach lists the terminals; --new, --name and a "
                     "command start one\n";
        return 2;
    }
    if (options.attachNew && (list || stop)) {
        std::cerr << "--new cannot be combined with --daemon-list or --daemon-stop\n";
        return 2;
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
        if (implicitNew && options.host.empty()) {
            // One line when terminals already run here, so that they are not
            // forgotten.
            std::string error;
            const auto socket = options.socketPath.empty()
                                    ? mtd::newestDaemonSocket()
                                    : options.socketPath;
            if (const auto info = frontend::probeDaemon(socket, error, 1000);
                info && info->status && info->status->sessions > 0 &&
                mtd::compatibility(mtd::ownVersion(), info->version) !=
                    mtd::Compatibility::Incompatible) {
                const auto n = info->status->sessions;
                std::cerr << n << (n == 1 ? " terminal runs" : " terminals run")
                          << ": matmux --attach\n";
            }
        }
        return runAttachTty(options);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
