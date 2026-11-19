#include "eggs.h"

#include <utility>

namespace fakeos {
namespace {

using T = Egg::Trigger;

// Chained construction of table rows: egg(id, trigger,
// match).print(...).once().
class Builder {
public:
    Builder(const char *id, T trigger, const char *match) {
        egg_ = {id, trigger, match, "", {}, "", true, false, ""};
    }
    Builder &print(const char *text) {
        egg_.print = text;
        return *this;
    }
    Builder &create(std::string path, std::string data, std::uint16_t mode) {
        egg_.create.push_back({std::move(path), std::move(data), mode});
        return *this;
    }
    Builder &launch(const char *program) {
        egg_.launch = program;
        return *this;
    }
    // Fires once per world; `again` is printed on later tries.
    Builder &once(const char *again = "") {
        egg_.once = true;
        egg_.repeatPrint = again;
        return *this;
    }
    // The line still reaches the shell afterwards.
    Builder &passThrough() {
        egg_.replaces = false;
        return *this;
    }
    operator Egg() const {
        return egg_;
    }

private:
    Egg egg_;
};

Builder egg(const char *id, T trigger, const char *match) {
    return Builder(id, trigger, match);
}

} // namespace

const std::vector<Egg> &eggs() {
    static const std::vector<Egg> table = {
        egg("sandwich-sudo", T::Line, "sudo make me a sandwich")
            .print("Okay.\n"),
        egg("sandwich", T::Line, "make me a sandwich")
            .print("What? Make it yourself.\n"),
        egg("sudo", T::Command, "sudo")
            .print("guest is not in the sudoers file. This incident will be "
                   "reported.\n"),
        egg("xyzzy", T::Line, "xyzzy")
            .print("A hollow voice says \"Fool.\"\nSomewhere in /usr/games, "
                   "something rustles.\n")
            .create("/usr/games/plasma", "#!builtin gradient --pills\n", 0755)
            .once("Nothing happens.\n"),
        egg("hello", T::Command, "hello")
            .print("Hello, guest. The system hums quietly.\n"),
        // Clicks on words a program drew (mouse reporting): the pills of the
        // plasma program. The blue one ends the story.
        egg("pill-blue", T::Click, "gradient/blue")
            .print("You close your eyes. When you open them you are at your "
                   "own keyboard,\n"
                   "and you believe whatever you want to believe.\n"),
    };
    return table;
}

namespace {
std::string trimmed(const std::string &line) {
    const auto begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    return line.substr(begin, line.find_last_not_of(" \t") - begin + 1);
}
} // namespace

const Egg *matchClickEgg(const World &,
                         const std::string &program,
                         const std::string &word) {
    const auto key = program + "/" + word;
    for (const auto &candidate : eggs()) {
        if (candidate.trigger == T::Click && key == candidate.match) {
            return &candidate;
        }
    }
    return nullptr;
}

const Egg *matchEgg(const World &, const std::string &line) {
    const auto text = trimmed(line);
    if (text.empty()) {
        return nullptr;
    }
    const auto firstWord = text.substr(0, text.find_first_of(" \t"));
    for (const auto &candidate : eggs()) {
        const std::string match = candidate.match;
        switch (candidate.trigger) {
        case T::Line:
            if (text == match) {
                return &candidate;
            }
            break;
        case T::Prefix:
            if (text.compare(0, match.size(), match) == 0) {
                return &candidate;
            }
            break;
        case T::Command:
            if (firstWord == match) {
                return &candidate;
            }
            break;
        case T::Click:
            break; // Only through matchClickEgg.
        }
    }
    return nullptr;
}

bool applyEgg(World &world, const Egg &egg, std::string &terminal) {
    if (egg.once && world.flags.count(egg.id)) {
        if (*egg.repeatPrint) {
            terminal += egg.repeatPrint;
            return true;
        }
        return false;
    }
    world.flags.insert(egg.id);
    terminal += egg.print;
    for (const auto &file : egg.create) {
        world.fs.put(file.path, file.data, "root", file.mode, world.unixNow());
    }
    if (*egg.launch) {
        world.launch = World::Launch{egg.launch, {}};
    }
    return true;
}

} // namespace fakeos
