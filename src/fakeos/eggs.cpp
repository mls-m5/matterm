#include "eggs.h"

namespace fakeos {

const std::vector<Egg> &eggs() {
    using T = Egg::Trigger;
    static const std::vector<Egg> table = {
        {"sandwich-sudo",
         T::Line,
         "sudo make me a sandwich",
         "Okay.\n",
         {},
         "",
         true,
         false,
         ""},
        {"sandwich",
         T::Line,
         "make me a sandwich",
         "What? Make it yourself.\n",
         {},
         "",
         true,
         false,
         ""},
        {"sudo",
         T::Command,
         "sudo",
         "guest is not in the sudoers file. This incident will be reported.\n",
         {},
         "",
         true,
         false,
         ""},
        {"xyzzy",
         T::Line,
         "xyzzy",
         "A hollow voice says \"Fool.\"\nSomewhere in /usr/games, something "
         "rustles.\n",
         {{"/usr/games/plasma", "#!builtin gradient\n", 0755}},
         "",
         true,
         true,
         "Nothing happens.\n"},
        {"hello",
         T::Command,
         "hello",
         "Hello, guest. The system hums quietly.\n",
         {},
         "",
         true,
         false,
         ""},
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

const Egg *matchEgg(const World &, const std::string &line) {
    const auto text = trimmed(line);
    if (text.empty()) {
        return nullptr;
    }
    const auto firstWord = text.substr(0, text.find_first_of(" \t"));
    for (const auto &egg : eggs()) {
        const std::string match = egg.match;
        switch (egg.trigger) {
        case Egg::Trigger::Line:
            if (text == match)
                return &egg;
            break;
        case Egg::Trigger::Prefix:
            if (text.compare(0, match.size(), match) == 0)
                return &egg;
            break;
        case Egg::Trigger::Command:
            if (firstWord == match)
                return &egg;
            break;
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
