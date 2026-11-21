#pragma once

#include <string>
#include <vector>

// Native entry-point adapter; owns SDL lifecycle after starting the PTY.
int runPtyFrontend(const char *font,
                   int fontSize,
                   const std::vector<std::string> &arguments,
                   const std::string &term,
                   bool smokeTest,
                   const char *recordPath,
                   bool smoothCursor = false,
                   bool tabsSmoke = false);
