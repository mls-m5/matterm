#pragma once

#include "world.h"

#include <string>
#include <vector>

namespace fakeos {

// The streams of one command in a pipeline. Output uses plain "\n"; the shell
// converts line ends for the terminal.
struct CommandIo {
    std::string in;
    std::string out;
    std::string err;
    bool tty = false; // `out` goes straight to the terminal (colors, columns).
};

using CommandFn = int (*)(World &world,
                          const std::vector<std::string> &args,
                          CommandIo &io);

struct CommandInfo {
    const char *name;
    CommandFn run;
    // Shell builtins are found without a file; the others exist as /bin files,
    // so they vanish with the file (and `reset` brings them back).
    bool builtin;
    const char *summary;
};

const std::vector<CommandInfo> &commands();
const CommandInfo *findCommand(const std::string &name);
// Whether the command exists in this world (`chat` needs a configured server).
bool commandAvailable(const World &w, const CommandInfo &c);

// Text helpers shared with the shell.
std::string unixDate(std::int64_t seconds, const char *format); // UTC.
std::vector<std::string> splitLines(const std::string &text);
// Shell-style wildcard match ('*', '?', '[abc]', backslash escapes).
bool globMatch(const std::string &pattern, const std::string &text);

} // namespace fakeos
