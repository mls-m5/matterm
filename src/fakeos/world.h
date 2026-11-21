#pragma once

#include "hardware.h"
#include "keys.h"
#include "types.h"
#include "virtualfs.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fakeos {

// How the chat reaches a server. The core opens no sockets: the host provides
// these (the browser: a JavaScript WebSocket) and reports what happens through
// System::chatOpened(), chatLine() and chatClosed(). Empty: no chat; the `chat`
// command does not exist then.
struct ChatLink {
    std::string server; // Shown in the notice, e.g. "chat.example.org".
    std::function<void()> open;
    std::function<void(const std::string &)> send;
    std::function<void()> close;
    bool available() const {
        return bool(open) && bool(send) && bool(close);
    }
};

// Everything a command, a program or an easter egg may read or change: the
// virtual file system, the shell's state, the clock and the discoveries made so
// far. There are no globals; one World belongs to one System.
struct World {
    VirtualFs fs;
    std::string user = "guest";
    std::string hostname = "matterm";
    // Running as the standalone Mattrix OS program, not inside matterm: the
    // intro and `about` use the OS name only.
    bool standalone = false;
    std::string cwd = "/home/guest";
    std::map<std::string, std::string> env;
    int lastStatus = 0;
    std::vector<std::string> history;
    int columns = 80, rows = 24;
    Capabilities caps;
    // The terminal's default background: animations fade to it, so leaving the
    // alternate screen shows no jump.
    Rgb background{30, 30, 30};
    Rng rng;
    Time now{0};               // Time since the system started.
    std::int64_t bootUnix = 0; // Unix seconds at start; see unixNow().
    // Names of discoveries (easter eggs found, programs unlocked).
    std::set<std::string> flags;
    // Pips reported by programs (typed text) are collected here and handed to
    // Config::onBeep by the System; nothing is collected when sound is off.
    bool sound = false;          // Pips are on (`sound on|off`).
    bool soundAvailable = false; // The host can play them.
    // The dramatic typing speed (revealed text and its pips): `slow on|off`.
    bool slow = false;
    // How grumpy the system is about repeated greetings, by group (eggs.h).
    std::map<std::string, int> moods;
    bool rebootRequest = false;
    // A yes/no question the system waits to have answered with one key; yes
    // crashes the machine (the rm -rf --no-preserve-root egg).
    std::string crashQuestion;
    ChatLink chat;
    bool chatQuestion = false; // `chat` asked "Connect? [y/N]" and waits.
    bool slowAsked =
        false; // `slow on` was typed: reveal even if the host set no pace.
    std::vector<Beep> beeps;
    void beep(double frequency, int durationMs, double volume = 0.3) {
        if (sound) {
            beeps.push_back({frequency, durationMs, volume, Time{0}});
        }
    }
    // Set by commands, handled by the System after the command line ran.
    std::optional<int> exitRequest;
    bool resetRequest = false;
    struct Launch {
        std::string name;
        std::vector<std::string> args;
        std::string input; // Text for the program (a pager's file), if any.
    };
    // The text handed to the program that is starting (read in its begin()).
    std::string programInput;
    std::optional<Launch> launch;

    std::int64_t unixNow() const {
        return bootUnix +
               std::chrono::duration_cast<std::chrono::seconds>(now).count();
    }
    // Seconds since the system booted (see hardware::kBootUnix), by the host's
    // clock plus the session's time: it only ever counts up.
    std::int64_t uptimeSeconds() const {
        return std::max<std::int64_t>(0, unixNow() - hardware::kBootUnix);
    }
    std::string home() const {
        const auto it = env.find("HOME");
        return it == env.end() ? "/" : it->second;
    }
    std::string path(const std::string &relative) const {
        return VirtualFs::normalize(cwd, relative);
    }
};

// Fills `world` with the initial virtual system for its user and clock.
void seedWorld(World &world);
// The welcome text for the current typing-speed setting (also /etc/motd).
std::string motdText(const World &world);

} // namespace fakeos
