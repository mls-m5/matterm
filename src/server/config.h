#pragma once
#include "server.h"
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Configuration of matterm-server: built-in defaults, overridden by a JSON
// file, overridden by command-line flags (docs/SERVER_CONFIG.md).
namespace server {
// Everything the executable reads from the file and the flags.
struct Settings {
    ServerConfig server;
    // Path of the credentials file; empty when there is none.
    std::string credentialsPath;
    // True when credentialsPath is only the default next to the config file,
    // so that a missing file is not an error.
    bool credentialsDefaulted = false;
};
// Every configuration problem. what() names the file position and the key path
// (or the flag), never a value.
class ConfigError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Environment lookup, injectable for tests; returns nullptr for an unset name.
using GetEnv = std::function<const char *(const char *)>;
// $XDG_CONFIG_HOME/matterm, else $HOME/.config/matterm (only absolute values
// count). Empty when neither is usable.
std::string defaultConfigDirectory(const GetEnv &getenv);

// Applies one JSON document on top of `settings`; all or nothing. `label`
// names the source in error messages. Members whose name starts with `_` are
// ignored at any depth. Unknown keys, wrong types, out-of-range values,
// duplicate keys and malformed JSON are errors.
void applyConfigText(std::string_view text,
                     Settings &settings,
                     const std::string &label = "config");
enum class ConfigFile { Optional, Required };
// Reads and applies a file after checking that it is a regular file (not a
// symbolic link), owned by the current user and not writable by group or
// others. Returns false only for an Optional file that does not exist.
bool loadConfigFile(const std::string &path,
                    ConfigFile mode,
                    Settings &settings);
// Checks the settings as a whole (also after command-line overrides).
void validateSettings(const Settings &settings);
// The effective settings as JSON that applyConfigText accepts again. The
// password hash is never printed; `password_hash` shows a placeholder.
std::string printConfig(const Settings &settings);

struct CommandLine {
    std::optional<std::string> configPath, listen, origin, credentials,
        staticDir, trustedProxy, daemonSocket;
    std::optional<unsigned short> port;
    std::optional<unsigned> trustedProxyHops;
    bool allowPlainHttp = false;
    bool exposeLocalSessions = false;
    bool allowRemoteListen = false;
    bool setPassword = false;
    bool printConfig = false;
    bool help = false;
    // Program and arguments after `--`.
    std::optional<std::vector<std::string>> program;
};
// `arguments` excludes argv[0].
CommandLine parseCommandLine(const std::vector<std::string> &arguments);
void applyCommandLine(const CommandLine &commandLine, Settings &settings);
// Defaults, then the config file (the default one is optional, one named with
// --config is required), then the flags, then validateSettings. Without an
// explicit credentials path the default is `credentials` next to the config
// file.
Settings resolveSettings(const CommandLine &commandLine, const GetEnv &getenv);
} // namespace server
