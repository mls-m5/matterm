#pragma once

// The names of the matterm-daemon and matmux files that
// --daemon-install copies to another computer, and which of the old ones to
// remove afterwards (docs/REMOTE_DAEMON.md "Installing"). A name says what is
// inside: matterm-daemon-<format>.<breaking>.<additive>-<hash8>. Pure.
#include "version.h"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace frontend {

inline std::string installedName(const std::string &program,
                                 const mtd::Version &v) {
    return program + "-" + std::to_string(v.format) + "." +
           std::to_string(v.breaking) + "." + std::to_string(v.additive) + "-" +
           v.hash.substr(0, 8);
}

struct InstalledFile {
    std::string name;
    std::string program; // "matterm-daemon" or "matmux".
    mtd::Version version;
    bool running = false; // A process runs this file.
};

// Reads "<program>-F.B.A-hhhhhhhh" (8 lower-case hex digits); nullopt for
// anything else, so nothing but our own files is ever a candidate for removal.
inline std::optional<InstalledFile> parseInstalledName(
    const std::string &name) {
    for (const std::string program : {"matterm-daemon", "matmux"}) {
        const std::string prefix = program + "-";
        if (name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto rest = name.substr(prefix.size());
        const auto dash = rest.find('-');
        if (dash == std::string::npos || rest.size() - dash - 1 != 8) {
            return std::nullopt;
        }
        const auto hash = rest.substr(dash + 1);
        if (!std::all_of(hash.begin(), hash.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            return std::nullopt;
        }
        // The version part "F.B.A": reuse the parser of the Welcome text.
        const auto version = mtd::findVersionInText(rest.substr(0, dash));
        if (!version || mtd::toString(*version) != rest.substr(0, dash)) {
            return std::nullopt;
        }
        InstalledFile file;
        file.name = name;
        file.program = program;
        file.version = *version;
        file.version.hash = hash;
        return file;
    }
    return std::nullopt;
}

// Lines "name 0|1" (1: a process runs it) as the remote shell prints them;
// lines that are not one of our names are dropped.
inline std::vector<InstalledFile> parseInstalledListing(
    const std::string &text) {
    std::vector<InstalledFile> files;
    std::size_t from = 0;
    while (from < text.size()) {
        auto end = text.find('\n', from);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(from, end - from);
        from = end + 1;
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto space = line.rfind(' ');
        if (space == std::string::npos || space + 2 != line.size() ||
            (line.back() != '0' && line.back() != '1')) {
            continue;
        }
        if (auto file = parseInstalledName(line.substr(0, space))) {
            file->running = line.back() == '1';
            files.push_back(std::move(*file));
        }
    }
    return files;
}

// Which files to remove once `current` (names) are in place: of each program
// everything older than the current version except the newest of those (a
// fallback), and never a running file or a newer version than the current one.
inline std::vector<std::string> filesToRemove(
    const std::vector<InstalledFile> &found,
    const std::set<std::string> &current) {
    std::vector<std::string> remove;
    for (const std::string program : {"matterm-daemon", "matmux"}) {
        const InstalledFile *now = nullptr;
        for (const auto &f : found) {
            if (f.program == program && current.count(f.name)) {
                now = &f;
            }
        }
        if (!now) {
            continue;
        }
        const auto key = [](const InstalledFile &f) {
            return std::make_tuple(f.version.format,
                                   f.version.breaking,
                                   f.version.additive,
                                   f.version.hash);
        };
        std::vector<const InstalledFile *> others;
        for (const auto &f : found) {
            if (f.program == program && !current.count(f.name) &&
                std::make_tuple(
                    f.version.format, f.version.breaking, f.version.additive) <=
                    std::make_tuple(now->version.format,
                                    now->version.breaking,
                                    now->version.additive)) {
                others.push_back(&f);
            }
        }
        std::sort(
            others.begin(), others.end(), [&](const auto *a, const auto *b) {
                return key(*a) > key(*b);
            });
        for (std::size_t i = 1; i < others.size();
             ++i) { // [0] is the fallback.
            if (!others[i]->running) {
                remove.push_back(others[i]->name);
            }
        }
    }
    return remove;
}

} // namespace frontend
