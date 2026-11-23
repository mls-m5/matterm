#pragma once

// The version of the daemon's control protocol: format.breaking.additive plus
// the build hash (docs/tasks/daemon-versioning.md, docs/PROTOCOL.md "Control
// version").
//
//  format    the shape of the handshake (ControlHello/ControlWelcome and
//  GetStatus/Shutdown,
//            0x40, 0xc0, 0x43, 0x44). Frozen: today's bytes stay readable by
//            every version, so any version can say who it is and be asked to
//            stop. Almost never changes.
//  breaking  a different value: the client refuses to connect (and offers a new
//  daemon next to
//            the old one). It is the number in ControlHello/ControlWelcome
//            `version`, as before.
//  additive  a different value: only a note; the newer side handles the older
//  one. hash      the git commit of the build, for messages and lists; it never
//  decides anything.
//
// The Welcome carries it in its free text `build`
// ("matterm-daemon 1.3.1+abc1234"), so the handshake bytes did not change. A
// daemon that predates this prints no such number: its Welcome version is its
// breaking number and its additive number is 0. Pure, no I/O.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mtd {

struct Version {
    std::uint16_t format = 1;
    std::uint16_t breaking = 3;
    std::uint16_t additive = 0;
    std::string hash; // May be empty (unknown).
    friend bool operator==(const Version &a, const Version &b) {
        return a.format == b.format && a.breaking == b.breaking &&
               a.additive == b.additive;
    }
};

// "1.3.1" or "1.3.1+abc1234".
inline std::string toString(const Version &v) {
    std::string s = std::to_string(v.format) + "." +
                    std::to_string(v.breaking) + "." +
                    std::to_string(v.additive);
    if (!v.hash.empty())
        s += "+" + v.hash;
    return s;
}

// The last word of `text` of the form N.N.N[+hash] (the hash up to 64
// characters), if any.
inline std::optional<Version> findVersionInText(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0) {
        const std::size_t begin = text.rfind(' ', end - 1);
        const std::size_t start =
            begin == std::string_view::npos ? 0 : begin + 1;
        const std::string_view word = text.substr(start, end - start);
        unsigned long part[3] = {0, 0, 0};
        int index = 0;
        std::size_t i = 0;
        bool ok = !word.empty();
        for (; ok && i < word.size() && word[i] != '+'; ++i) {
            const char c = word[i];
            if (c == '.') {
                if (++index > 2)
                    ok = false;
            }
            else if (c >= '0' && c <= '9') {
                part[index] = part[index] * 10 + unsigned(c - '0');
                if (part[index] > 65535)
                    ok = false;
            }
            else {
                ok = false;
            }
        }
        if (ok && index == 2 && i > 0 && word[i - 1] != '.' &&
            (i == word.size() || word.substr(i + 1).size() <= 64)) {
            Version v;
            v.format = std::uint16_t(part[0]);
            v.breaking = std::uint16_t(part[1]);
            v.additive = std::uint16_t(part[2]);
            if (i < word.size())
                v.hash = std::string(word.substr(i + 1));
            return v;
        }
        if (begin == std::string_view::npos)
            break;
        end = begin;
    }
    return std::nullopt;
}

// What a ControlWelcome says. `build` is its text, `welcomeVersion` its number
// field. The number in the handshake is the frozen part: if the text disagrees
// with it, the handshake wins.
inline Version versionFromWelcome(std::string_view build,
                                  std::uint16_t welcomeVersion) {
    Version legacy;
    legacy.breaking = welcomeVersion;
    const auto found = findVersionInText(build);
    return found && found->breaking == welcomeVersion ? *found : legacy;
}

// The version this build speaks, with its commit hash ("unknown" when it was
// not built from git).
Version ownVersion();
// The text for ControlWelcome::build.
std::string welcomeBuild(std::string_view program, const Version &v);

enum class Compatibility {
    Same,        // Same format, breaking and additive.
    DaemonOlder, // Same breaking, the daemon has a lower additive number: note.
    DaemonNewer, // Same breaking, the daemon has a higher additive number:
                 // note.
    Incompatible, // Different format or breaking number: refuse.
};

inline Compatibility compatibility(const Version &mine, const Version &daemon) {
    if (mine.format != daemon.format || mine.breaking != daemon.breaking)
        return Compatibility::Incompatible;
    if (daemon.additive < mine.additive)
        return Compatibility::DaemonOlder;
    if (daemon.additive > mine.additive)
        return Compatibility::DaemonNewer;
    return Compatibility::Same;
}

} // namespace mtd
