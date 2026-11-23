#pragma once

// The command-line options of matterm, matterm-server, matterm-daemon and
// matmux as data, and the shell completion scripts generated from them
// (`matterm --completion bash|zsh|fish`). One table per program: the completion
// cannot drift from it, and test/clioptions_test.cpp compares each table with
// what the program's own source parses and prints.
#include <cstddef>
#include <string>
#include <vector>

namespace frontend::cli {

enum class Value { None, File, Dir, Host, Number, Choices, Text };

struct Option {
    const char *name;         // "--font-size"
    const char *shortName;    // "-h" or ""
    Value value;              // what follows it, if anything
    const char *choices;      // Value::Choices: the words, space separated
    const char *help;         // one line, no quotes
    bool hidden;              // not completed (self-tests)
};

struct Program {
    const char *name;
    const char *positionalGlob; // Files completed for a bare word ("*.ttf"), or "".
    std::vector<Option> options;
};

inline const std::vector<Program> &programs() {
    static const std::vector<Program> all = {
        {"matterm",
         "*.ttf",
         {
             {"--help",
              "-h",
              Value::None,
              "",
              "Show help without opening a window",
              false},
             {"--font-size",
              "",
              Value::Number,
              "",
              "Font size in pixels, 1..128",
              false},
             {"--term",
              "",
              Value::Choices,
              "xterm-256color xterm screen-256color tmux-256color linux vt100",
              "TERM for the child",
              false},
             {"--daemon",
              "",
              Value::None,
              "",
              "Keep the terminals in matterm-daemon",
              false},
             {"--no-daemon",
              "",
              Value::None,
              "",
              "Run the programs in this process",
              false},
             {"--daemon-socket",
              "",
              Value::File,
              "",
              "Socket of the daemon",
              false},
             {"--daemon-host",
              "",
              Value::Host,
              "",
              "Use the daemon of another computer through ssh",
              false},
             {"--daemon-install",
              "",
              Value::None,
              "",
              "Use our own copy of matterm-daemon on that computer",
              false},
             {"--yes", "", Value::None, "", "Copy it without asking", false},
             {"--ssh-arg",
              "",
              Value::Text,
              "",
              "Extra argument for ssh",
              false},
             {"--snap-scroll",
              "",
              Value::None,
              "",
              "Scrolling comes to rest on whole rows",
              false},
             {"--tab-activity",
              "",
              Value::Choices,
              "icon pulse off",
              "What a working tab shows",
              false},
             {"--no-tab-pulse",
              "",
              Value::None,
              "",
              "Same as tab-activity off",
              false},
             {"--system-frame",
              "",
              Value::None,
              "",
              "Use the window frame of the desktop, not our own",
              false},
             {"--borderless",
              "",
              Value::None,
              "",
              "The default: our own frame (accepted for old command lines)",
              false},
             {"--window-test",
              "",
              Value::None,
              "",
              "Print which window calls work here",
              false},
             {"--attach-tty",
              "",
              Value::None,
              "",
              "Take over a running terminal in this terminal (no window)",
              false},
             {"--new",
              "",
              Value::None,
              "",
              "With --attach-tty: start a terminal and attach to it",
              false},
             {"--name",
              "",
              Value::Text,
              "",
              "With --new: a name for the terminal",
              false},
             {"--daemon-list",
              "",
              Value::None,
              "",
              "List the daemons of this user (socket, pid, version, terminals)",
              false},
             {"--daemon-stop",
              "",
              Value::None,
              "",
              "Stop a daemon after asking (any version)",
              false},
             {"--smooth-cursor",
              "",
              Value::None,
              "",
              "Animate cursor movement",
              false},
             {"--record-output",
              "",
              Value::File,
              "",
              "Record raw PTY output to a new file",
              false},
             {"--input",
              "",
              Value::File,
              "",
              "Play a VT file without a shell",
              false},
             {"--showcase",
              "",
              Value::None,
              "",
              "Show the built-in VT example",
              false},
             {"--completion",
              "",
              Value::Choices,
              "bash zsh fish",
              "Print the shell completion script",
              false},
             {"--smoke-test", "", Value::None, "", "Hidden VT self-test", true},
             {"--pty-smoke-test",
              "",
              Value::None,
              "",
              "Hidden PTY self-test",
              true},
             {"--pty-tabs-smoke-test",
              "",
              Value::None,
              "",
              "Hidden tabs self-test",
              true},
             {"--pty-layout-smoke-test",
              "",
              Value::None,
              "",
              "Hidden detach-tab self-test",
              true},
         }},
        {"matterm-server",
         "",
         {
             {"--help", "", Value::None, "", "Show help", false},
             {"--config", "", Value::File, "", "Config file", false},
             {"--print-config",
              "",
              Value::None,
              "",
              "Print the settings in effect",
              false},
             {"--listen", "", Value::Text, "", "Address to listen on", false},
             {"--port", "", Value::Number, "", "Port", false},
             {"--origin", "", Value::Text, "", "Allowed Origin URL", false},
             {"--credentials", "", Value::File, "", "Credentials file", false},
             {"--static-dir",
              "",
              Value::Dir,
              "",
              "Folder of the web page",
              false},
             {"--allow-plain-http",
              "",
              Value::None,
              "",
              "Accept plain http",
              false},
             {"--trusted-proxy",
              "",
              Value::Choices,
              "none any",
              "Trust X-Forwarded headers from this proxy",
              false},
             {"--trusted-proxy-hops",
              "",
              Value::Number,
              "",
              "Proxies in front, 0..16",
              false},
             {"--expose-local-sessions",
              "",
              Value::None,
              "",
              "Offer shared terminals of the local daemon",
              false},
             {"--daemon-socket",
              "",
              Value::File,
              "",
              "Socket of the local daemon",
              false},
             {"--allow-remote-listen",
              "",
              Value::None,
              "",
              "Allow exposure beyond loopback",
              false},
             {"--set-password", "", Value::None, "", "Set the password", false},
             {"--setup-credentials",
              "",
              Value::None,
              "",
              "Set the password (alias)",
              false},
         }},
        {"matterm-daemon",
         "",
         {
             {"--help", "", Value::None, "", "Show help", false},
             {"--daemon-socket",
              "",
              Value::File,
              "",
              "Socket to listen on",
              false},
             {"--idle-seconds",
              "",
              Value::Number,
              "",
              "Exit after this long with nothing in it",
              false},
             {"--daemonize",
              "",
              Value::None,
              "",
              "Detach from the terminal",
              false},
             {"--ensure",
              "",
              Value::None,
              "",
              "Start it if it is not running",
              false},
             {"--print-socket",
              "",
              Value::None,
              "",
              "Print the socket path and exit",
              false},
             {"--version",
              "",
              Value::None,
              "",
              "Print the control version and exit",
              false},
         }},
        {"matmux",
         "",
         {
             {"--help", "-h", Value::None, "", "Show help", false},
             {"--version",
              "",
              Value::None,
              "",
              "Print the control version and exit",
              false},
             {"--attach",
              "",
              Value::None,
              "",
              "List the terminals in the daemon and take one over",
              false},
             {"--rendered",
              "",
              Value::None,
              "",
              "Draw the terminal cell by cell (alternate screen, status line) instead of passing its output through",
              false},
             {"--new",
              "",
              Value::None,
              "",
              "Start a terminal (your shell or a command) and attach to it",
              false},
             {"--name",
              "",
              Value::Text,
              "",
              "With --new: a name for the terminal",
              false},
             {"--daemon-socket",
              "",
              Value::File,
              "",
              "Socket of the daemon",
              false},
             {"--daemon-host",
              "",
              Value::Host,
              "",
              "Use the daemon of another computer through ssh",
              false},
             {"--daemon-install",
              "",
              Value::None,
              "",
              "Use our own copy of matterm-daemon on that computer",
              false},
             {"--yes", "", Value::None, "", "Copy it without asking", false},
             {"--ssh-arg",
              "",
              Value::Text,
              "",
              "Extra argument for ssh",
              false},
             {"--daemon-list",
              "",
              Value::None,
              "",
              "List the daemons of this user",
              false},
             {"--daemon-stop",
              "",
              Value::None,
              "",
              "Stop a daemon after asking",
              false},
         }},
    };
    return all;
}

namespace detail {

inline std::string clean(const std::string &s) { // Help text inside quotes of three shells.
    std::string out;
    for (const char c : s) {
        out += (c == '\'' || c == '"' || c == '[' || c == ']' || c == ':' || c == '\\' || c == '`' || c == '$') ? ' ' : c;
    }
    return out;
}
inline std::string wordList(const Program &p, bool withShort = true) {
    std::string out;
    for (const auto &o : p.options) {
        if (o.hidden) continue;
        out += std::string(out.empty() ? "" : " ") + o.name;
        if (withShort && o.shortName[0] != '\0') out += std::string(" ") + o.shortName;
    }
    return out;
}
inline std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string out;
    for (const auto &s : v) out += (out.empty() ? "" : sep) + s;
    return out;
}
// ~/.ssh/config Host names without patterns (shell, same in all three).
inline const char *hostsCommand() {
    return "awk '/^[ \\t]*[Hh][Oo][Ss][Tt][ \\t]/ { for (i = 2; i <= NF; i++) if ($i !~ /[*?!]/) print $i }' "
           "\"$HOME/.ssh/config\" 2>/dev/null";
}
} // namespace detail

inline std::string bashCompletion() {
    using namespace detail;
    std::string o =
        "# bash completion for matterm, matterm-server, matterm-daemon and "
        "matmux (generated by matterm --completion bash)\n";
    o += std::string("_matterm_hosts() { ") + hostsCommand() + "; }\n";
    for (const auto &p : programs()) {
        std::string fn = "_" + std::string(p.name);
        for (char &c : fn) if (c == '-') c = '_';
        o += fn + "() {\n  local cur=\"${COMP_WORDS[COMP_CWORD]}\" prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n  COMPREPLY=()\n"
             "  # --opt=value: bash splits at the =\n"
             "  if [[ \"$prev\" == = ]]; then prev=\"${COMP_WORDS[COMP_CWORD-2]}\"; elif [[ \"$cur\" == = ]]; then cur=\"\"; fi\n"
             "  case \"$prev\" in\n";
        for (const auto &opt : p.options) {
            if (opt.hidden || opt.value == Value::None) continue;
            o += std::string("    ") + opt.name + ")\n      ";
            switch (opt.value) {
            case Value::File: o += "mapfile -t COMPREPLY < <(compgen -f -- \"$cur\")"; break;
            case Value::Dir: o += "mapfile -t COMPREPLY < <(compgen -d -- \"$cur\")"; break;
            case Value::Host: o += "mapfile -t COMPREPLY < <(compgen -W \"$(_matterm_hosts)\" -- \"$cur\")"; break;
            case Value::Choices: o += std::string("mapfile -t COMPREPLY < <(compgen -W \"") + opt.choices + "\" -- \"$cur\")"; break;
            default: o += ":"; break; // Number, Text: nothing to offer.
            }
            o += "\n      return 0 ;;\n";
        }
        o += "  esac\n  if [[ \"$cur\" == -* ]]; then\n    mapfile -t COMPREPLY < <(compgen -W \"" + wordList(p) + "\" -- \"$cur\")\n";
        if (p.positionalGlob[0] != '\0') {
            o += std::string("  else\n    mapfile -t COMPREPLY < <(compgen -f -X '!") + p.positionalGlob +
                 "' -- \"$cur\"; compgen -d -- \"$cur\")\n";
        }
        o += "  fi\n  return 0\n}\n";
        o += std::string("complete -o filenames -F ") + fn + " " + p.name + "\n";
    }
    return o;
}

inline std::string zshCompletion() {
    using namespace detail;
    std::string o = "#compdef matterm matterm-server matterm-daemon matmux\n# "
                    "zsh completion (generated by matterm --completion zsh)\n";
    o += std::string("_matterm_hosts() { local -a hosts; hosts=(${(f)\"$(") + hostsCommand() + ")\"}); _describe 'host' hosts }\n";
    o += "_matterm() {\n  case \"$service\" in\n";
    for (const auto &p : programs()) {
        o += std::string("  ") + p.name + ")\n    _arguments -s \\\n";
        for (const auto &opt : p.options) {
            if (opt.hidden) continue;
            std::string spec = std::string("'") + opt.name + "[" + clean(opt.help) + "]";
            switch (opt.value) {
            case Value::None: break;
            case Value::File: spec += ":file:_files"; break;
            case Value::Dir: spec += ":folder:_files -/"; break;
            case Value::Host: spec += ":host:_matterm_hosts"; break;
            case Value::Choices: spec += std::string(":value:(") + opt.choices + ")"; break;
            case Value::Number: spec += ":number:"; break;
            case Value::Text: spec += ":value:"; break;
            }
            o += "      " + spec + "' \\\n";
            if (opt.shortName[0] != '\0') o += std::string("      '") + opt.shortName + "[" + clean(opt.help) + "]' \\\n";
        }
        o += p.positionalGlob[0] != '\0' ? std::string("      '1:font:_files -g \"") + p.positionalGlob + "\"' \\\n      '*:command:_normal'\n"
                                         : std::string("      '*:argument:_normal'\n");
        o += "    ;;\n";
    }
    o += "  esac\n}\n_matterm \"$@\"\n";
    return o;
}

inline std::string fishCompletion() {
    using namespace detail;
    std::string o =
        "# fish completion for matterm, matterm-server, matterm-daemon and "
        "matmux (generated by matterm --completion fish)\n";
    o += std::string("function __matterm_hosts\n    ") + hostsCommand() + "\nend\n";
    for (const auto &p : programs()) {
        for (const auto &opt : p.options) {
            if (opt.hidden) continue;
            std::string line = std::string("complete -c ") + p.name + " -l " + (opt.name + 2);
            if (opt.shortName[0] != '\0') line += std::string(" -s ") + (opt.shortName + 1);
            switch (opt.value) {
            case Value::None: break;
            case Value::File: line += " -r -F"; break;
            case Value::Dir: line += " -r -a '(__fish_complete_directories)'"; break;
            case Value::Host: line += " -x -a '(__matterm_hosts)'"; break;
            case Value::Choices: line += std::string(" -x -a '") + opt.choices + "'"; break;
            case Value::Number:
            case Value::Text: line += " -x"; break;
            }
            o += line + " -d '" + clean(opt.help) + "'\n";
        }
        if (p.positionalGlob[0] != '\0') {
            o += std::string("complete -c ") + p.name + " -n '__fish_is_first_token' -a '(__fish_complete_suffix .ttf)' -d 'font'\n";
        }
    }
    return o;
}

// "bash", "zsh" or "fish"; empty for anything else.
inline std::string completionScript(const std::string &shell) {
    if (shell == "bash") return bashCompletion();
    if (shell == "zsh") return zshCompletion();
    if (shell == "fish") return fishCompletion();
    return {};
}

} // namespace frontend::cli
