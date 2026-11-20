#pragma once

#include "world.h"

#include <string>
#include <vector>

namespace fakeos {

// The pretend shell: tokenizer, expansion (variables, `~`, `*` `?` `[...]`),
// pipelines, `;` `&&` `||`, redirects and program lookup through the virtual
// file system. It writes nothing itself: run() returns what the terminal
// should show, and requests (exit, reset, a full-screen program) are left in
// the World for the System.
class Shell {
public:
    explicit Shell(World &world)
        : world_(world) {}

    // Runs one command line; the result has CR LF line ends.
    std::string run(const std::string &line);

    struct Completion {
        std::size_t start = 0;            // Offset of the word being completed.
        std::vector<std::string> options; // Replacement texts for that word.
    };
    // Completes the last word of `prefix` (the line up to the cursor).
    Completion complete(const std::string &prefix) const;

private:
    struct Segment {
        std::string text;
        char quote = 0; // 0 unquoted, '\'' or '"'.
    };
    struct Word {
        std::vector<Segment> segments;
    };
    struct Simple {
        std::vector<Word> words;
        bool hasIn = false, hasOut = false, append = false;
        Word in, out;
    };
    struct Item {
        std::vector<Simple> pipeline;
        enum class Join { First, Seq, And, Or } join = Join::First;
    };

    bool parse(const std::string &line,
               std::vector<Item> &items,
               std::string &error) const;
    std::vector<std::string> expand(const Word &word) const;
    std::string expandOne(const Word &word) const;
    std::string variable(const std::string &name) const;
    int runPipeline(const std::vector<Simple> &pipeline, std::string &terminal);
    int runCommand(const std::vector<std::string> &args,
                   struct CommandIo &io,
                   std::string &terminal);

    World &world_;
};

} // namespace fakeos
