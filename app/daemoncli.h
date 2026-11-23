#pragma once

#include "ptyfrontend.h"

// `matterm --daemon-list` and `--daemon-stop` (docs/REMOTE_DAEMON.md "Seeing
// and stopping daemons"), also in matterm-attach. Both return the exit code. A
// daemon is named by its socket:
// --daemon-socket picks one, else all of this user's sockets are looked at.
int runDaemonList(const DaemonOptions &daemon);
int runDaemonStop(const DaemonOptions &daemon);
