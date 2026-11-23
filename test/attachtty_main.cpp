// A program without SDL that is only `runAttachTty` (what matterm-attach will
// be built on): proves that the library needs no window libraries. Usage:
// attachtty_probe [--daemon-socket PATH].
#include "attachtty.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    DaemonOptions options;
    options.enabled = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--attach-tty") {
            continue; // So that the same test drives matterm and this.
        }
        if (arg == "--daemon-socket" && i + 1 < argc) {
            options.socketPath = argv[++i];
        }
        else {
            std::cerr << "usage: attachtty_probe [--daemon-socket PATH]\n";
            return 2;
        }
    }
    try {
        return runAttachTty(options);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
