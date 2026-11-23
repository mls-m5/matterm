#pragma once

#include "ptyfrontend.h"

// `matterm --attach-tty` (docs/tasks/attach-tty.md): takes over a terminal of
// the daemon in the terminal this program runs in. Uses DaemonOptions for
// --daemon-socket and --daemon-host. Returns the exit code; throws
// std::runtime_error for what the user must be told before anything is drawn.
int runAttachTty(const DaemonOptions &daemon);
