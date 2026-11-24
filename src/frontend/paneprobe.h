#pragma once

// What a pane is doing, as far as saving a session and closing safely need to
// know (docs/TABS_PLAN.md section 9): the folder of its foreground process and,
// when that is not a shell, the command line it was started with. Pure logic over
// a `ProcSource`, so it is tested with a fake; the Linux one reads /proc
// (src/platform/posix/procinfo.h). Nothing here starts a program: a saved command
// line is only ever shown, and `commandLine()` quotes it for a shell when the
// owner chooses to run it again.
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frontend {

struct ProcSource {
    virtual ~ProcSource() = default;
    virtual std::optional<std::string> cwd(int pid) const = 0;
    virtual std::optional<std::vector<std::string>> cmdline(int pid) const = 0;
};

struct PaneProbeLimits {
    static constexpr std::size_t kMaxArguments = 32;
    static constexpr std::size_t kMaxArgument = 1024;
    static constexpr std::size_t kMaxCommand = 4096; // All arguments together.
    static constexpr std::size_t kMaxPath = 4096;
};

struct PaneState {
    std::string cwd;                  // Absolute, empty when unknown.
    bool busy = false;                // The foreground process is not a shell.
    std::vector<std::string> command; // Its command line, empty if unknown or unsafe to keep.
    // Rich-list classification is independent of the conservative legacy busy
    // fallback. Shell foreground is NOT proof of an idle prompt.
    std::optional<bool> foregroundBusy;
};

inline bool isControl(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

// A path worth keeping: absolute, bounded, no control characters.
inline bool plainPath(std::string_view p) {
    if (p.empty() || p.front() != '/' || p.size() > PaneProbeLimits::kMaxPath) {
        return false;
    }
    return std::none_of(p.begin(), p.end(), [](char c) { return isControl(static_cast<unsigned char>(c)); });
}

// A command line that may be saved and offered again: bounded, a non-empty
// program name, and no control character anywhere (a newline in an argument would
// become a second command when typed into a shell).
inline bool plainCommand(const std::vector<std::string> &argv) {
    if (argv.empty() || argv.size() > PaneProbeLimits::kMaxArguments || argv.front().empty()) {
        return false;
    }
    std::size_t total = 0;
    for (const auto &a : argv) {
        total += a.size() + 1;
        if (a.size() > PaneProbeLimits::kMaxArgument || total > PaneProbeLimits::kMaxCommand ||
            std::any_of(a.begin(), a.end(), [](char c) { return isControl(static_cast<unsigned char>(c)); })) {
            return false;
        }
    }
    return true;
}

// argv[0] names a shell: "bash", "-zsh" (a login shell), "/usr/bin/fish".
inline bool isShellName(std::string_view argv0) {
    const auto slash = argv0.rfind('/');
    if (slash != std::string_view::npos) argv0.remove_prefix(slash + 1);
    if (!argv0.empty() && argv0.front() == '-') argv0.remove_prefix(1);
    for (const std::string_view s : {"sh", "bash", "zsh", "fish", "dash", "ksh", "tcsh", "csh", "ash", "nu",
                                      "elvish", "xonsh", "mksh", "yash"}) {
        if (argv0 == s) return true;
    }
    return false;
}

// `foreground` is the pane's foreground process group (0 if unknown), `shell` the
// process the pane started.
inline PaneState probePane(const ProcSource &source, int shell, int foreground) {
    PaneState state;
    const int pid = foreground > 0 ? foreground : shell;
    if (auto cwd = source.cwd(pid); cwd && plainPath(*cwd)) {
        state.cwd = std::move(*cwd);
    }
    else if (pid != shell) {
        if (auto fallback = source.cwd(shell); fallback && plainPath(*fallback)) {
            state.cwd = std::move(*fallback);
        }
    }
    const auto argv = source.cmdline(pid);
    if (argv && !argv->empty()) {
        if (foreground > 0 && !argv->front().empty())
            state.foregroundBusy = !isShellName(argv->front());
        if (!isShellName(argv->front())) {
            state.busy = true;
            if (plainCommand(*argv)) state.command = *argv;
        }
    }
    else if (pid != shell) {
        state.busy = true; // Something else runs and cannot be read.
    }
    return state;
}

// Shell quoting: single quotes, with ' written as '\''. Safe for plainCommand input.
inline std::string quoteArgument(std::string_view a) {
    const bool bare = !a.empty() && std::all_of(a.begin(), a.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '.' || c == '_' || c == '-' || c == '=' || c == ':' || c == ',' || c == '+' ||
               c == '@' || c == '%';
    });
    if (bare) return std::string(a);
    std::string out = "'";
    for (const char c : a) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// The text to type into a shell to run the command again (no newline: the owner
// presses Enter). Empty for a command that is not plainCommand().
inline std::string commandLine(const std::vector<std::string> &argv) {
    if (!plainCommand(argv)) return {};
    std::string out;
    for (const auto &a : argv) {
        if (!out.empty()) out += ' ';
        out += quoteArgument(a);
    }
    return out;
}

} // namespace frontend
