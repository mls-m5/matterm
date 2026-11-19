#pragma once

#include "system.h"

// Runs the pretend system in the terminal this process was started from:
// raw mode, standard input and output, SIGWINCH for resizes. Returns the exit
// code of the pretend session. The terminal is restored on every way out.
// Needs a terminal on stdin and stdout (otherwise prints why and returns 1).
int runTtyHost(fakeos::Config config);
