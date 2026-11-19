#pragma once

#include "world.h"

#include <string>
#include <vector>

namespace fakeos {

// Easter eggs are data: a trigger, and what happens. Adding one means adding a
// row to the table in eggs.cpp, nothing in the shell. Triggers on typed lines
// are checked before the shell parses the line. Other kinds of discovery work
// through the virtual file system instead (hidden files, programs that only
// exist after something was found) and through programs that change the World
// when they end. Clicking on screen is reserved for programs that turn on
// mouse reporting.
struct EggFile {
    std::string path;
    std::string data;
    std::uint16_t mode;
};

struct Egg {
    enum class Trigger {
        Line,    // The whole trimmed line equals `match`.
        Prefix,  // The line starts with `match`.
        Command, // The first word equals `match`.
        // A click, through mouse reporting, on text a program drew: `match` is
        // "program/word", for example "gradient/red" (the word the click landed
        // on, as drawn). The program ends; `print` is shown once the screen is
        // back and `launch` may start the next program.
        Click,
    };
    const char *id; // Also the World flag that records the discovery.
    Trigger trigger;
    const char *match;
    const char *print;           // Text for the terminal, "\n" ends lines.
    std::vector<EggFile> create; // Files made (as root) when it fires.
    const char *launch;          // A program to start, or empty.
    bool replaces;               // Skip the shell for this line.
    bool once;                   // Fires once per world.
    const char *repeatPrint;     // Printed instead after the first time.
};

const std::vector<Egg> &eggs();
// The first egg whose trigger matches the line, or null.
const Egg *matchEgg(const World &world, const std::string &line);
// The click egg for a word drawn by the named program, or null.
const Egg *matchClickEgg(const World &world,
                         const std::string &program,
                         const std::string &word);
// Performs the egg: prints, creates files, requests a program. Returns false
// when it had nothing to do (a `once` egg already found, without repeat text).
bool applyEgg(World &world, const Egg &egg, std::string &terminal);

} // namespace fakeos
