// matmux: matterm --attach-tty without the window libraries
// (docs/tasks/daemon-versioning.md). One file that can be copied to another
// computer and run over ssh (`ssh -t host matmux`): lists the terminals of the
// daemon, takes one over, and also lists and stops daemons.
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
           "  Without arguments: the list of the terminals in the daemon with "
           "\"New "
           "terminal\" chosen\n"
           "  (Enter starts one with your shell); when none runs, a new "
           "terminal "
           "at once (the daemon is\n"
           "  started when missing). Leave a terminal with Ctrl+B d: it keeps "
           "running.\n"
           "  --attach              the same list with the first terminal "
           "chosen "
           "(Ctrl+B l is the\n"
           "                        list again)\n"
           "  --daemon-socket PATH  the daemon on this socket (default: the "
           "compatible daemon in\n"
           "                        $XDG_RUNTIME_DIR/matterm)\n"
           "  --daemon-host HOST    the daemon of another computer, through an "
           "ssh tunnel\n"
           "  --daemon-install      with --daemon-host: use our own "
           "matterm-daemon there\n"
           "  --yes                 copy without asking\n"
           "  --ssh-arg ARG         an extra argument for ssh (repeatable)\n"
           "  --perf-log FILE       write a performance log (echo latency, "
           "round trip, bytes, parse time, CPU)\n"
           "                        one line per second to FILE (also: "
           "MATTERM_PERF_LOG=FILE)\n"
           "  --rendered            draw the terminal cell by cell in the "
           "alternate screen (status line,\n"
           "                        no host scrollback) instead of passing its "
           "output through\n"
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
    options.rendered = false; // Passthrough: the host terminal keeps its
                              // scrollback. --rendered: cell by cell.
    bool list = false, stop = false, attach = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--attach") {
            attach = true;
            continue;
        }
        if (arg == "--perf-log" && i + 1 < argc) {
            options.perfLog = argv[++i];
            continue;
        }
        if (arg == "--rendered") {
            options.rendered = true;
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
    // A name or a command is for a new terminal. Without any mode: the list
    // with New terminal chosen, or a new terminal at once when none runs.
    if (!attach && !list && !stop && !options.attachNew) {
        if (options.attachName.empty() && options.attachCommand.empty()) {
            options.attachDefault = true;
        }
        else {
            options.attachNew = true;
        }
    }
    if (attach && (!options.attachName.empty() ||
                   !options.attachCommand.empty() || options.attachNew)) {
        std::cerr << "--attach lists the terminals; --new, --name and a "
                     "command start one\n";
        return 2;
    }
    if (options.attachNew && (list || stop)) {
        std::cerr
            << "--new cannot be combined with --daemon-list or --daemon-stop\n";
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
        return runAttachTty(options);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
