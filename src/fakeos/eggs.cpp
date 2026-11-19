#include "eggs.h"

#include <algorithm>
#include <deque>
#include <utility>

namespace fakeos {
namespace {

using T = Egg::Trigger;

// Chained construction of table rows: egg(id, trigger,
// match).print(...).once().
class Builder {
public:
    Builder(const char *id, T trigger, const char *match) {
        egg_ = {id, trigger, match, "", {}, "", true, false, "", "", {}, ""};
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
    // Grumpier with every greeting of the group; the last step reboots.
    Builder &mood(const char *group, std::vector<const char *> steps) {
        egg_.group = group;
        egg_.steps = std::move(steps);
        return *this;
    }
    // Asks first; yes crashes the machine, anything else backs out.
    Builder &ask(const char *question) {
        egg_.crashQuestion = question;
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

// How the system answers a greeting, over and over; the last one is the end.
const std::vector<const char *> kGreetings = {
    "Hello, guest. The system hums quietly.",
    "Hello again, guest.",
    "Again? Did you forget?",
    "Is this a test?",
    "Please stop.",
    "I am a Professional Edition. Please act professionally.",
    "I can do this all day. Can you?",
    "please kill me...",
};

const std::vector<Egg> &eggs() {
    static const std::vector<Egg> table = [] {
        std::vector<Egg> rows = {
            egg("make-love-sudo", T::Line, "sudo make love")
                .print("make: don't know how to make love. Stop.\n"),
            egg("make-love", T::Line, "make love")
                .print("make: don't know how to make love. Stop.\n"),
            egg("sandwich-sudo", T::Line, "sudo make me a sandwich")
                .print("Okay.\n"),
            egg("sandwich", T::Line, "make me a sandwich")
                .print("What? Make it yourself.\n"),
            egg("sudo", T::Command, "sudo")
                .print(
                    "guest is not in the sudoers file. This incident will be "
                    "reported.\n"),
            egg("xyzzy", T::Line, "xyzzy")
                .print(
                    "A hollow voice says \"Fool.\"\nSomewhere in /usr/games, "
                    "something rustles.\n")
                .create(
                    "/usr/games/plasma", "#!builtin gradient --pills\n", 0755)
                .once("Nothing happens.\n"),
            egg("editor", T::Command, "vim")
                .print("There is no editor here, only echo and redirects. Nice "
                       "try.\n"),
            egg("editor-vi", T::Command, "vi")
                .print("There is no editor here, only echo and redirects. Nice "
                       "try.\n"),
            egg("editor-nano", T::Command, "nano")
                .print("There is no editor here, only echo and redirects. Nice "
                       "try.\n"),
            egg("editor-emacs", T::Command, "emacs")
                .print("There is no editor here, only echo and redirects. Nice "
                       "try.\n"),
            egg("forkbomb", T::Line, ":(){ :|:& };:")
                .print("matterm: fork: Resource temporarily unavailable. (This "
                       "system has more sense.)\n"),
            egg("answer", T::Line, "42")
                .print("That is the answer. The question is still open.\n"),
            egg("neo", T::Command, "neo").print("The Matrix has you.\n"),
            egg("hello", T::Command, "hello").mood("hello", kGreetings),
            egg("hi", T::Command, "hi").mood("hello", kGreetings),
            egg("hej", T::Command, "hej").mood("hello", kGreetings),
            // Clicks on words a program drew (mouse reporting): the pills of
            // the plasma program. The blue one ends the story.
            egg("pill-red", T::Click, "gradient/red")
                .print("You take the red pill. The rabbit hole opens.\n")
                .launch("matrix"),
            egg("pill-blue", T::Click, "gradient/blue")
                .print(
                    "You close your eyes. When you open them you are at your "
                    "own keyboard,\n"
                    "and you believe whatever you want to believe.\n"),
        };
        // rm -rf with --no-preserve-root, with or without sudo, in either
        // order: it asks, and yes ends badly.
        static std::deque<std::string>
            texts; // Stable addresses for the matches.
        for (const char *sudo : {"", "sudo "}) {
            for (const char *rm : {"rm -rf", "rm -fr"}) {
                for (const char *form :
                     {"--no-preserve-root /", "/ --no-preserve-root"}) {
                    texts.push_back(std::string(sudo) + rm + " " + form);
                    rows.push_back(
                        egg("crash", T::Line, texts.back().c_str())
                            .ask("Are you really really sure? (y/n)"));
                }
            }
        }
        // The questions come first: `sudo rm ...` must not be taken for plain
        // sudo.
        std::stable_partition(rows.begin(), rows.end(), [](const Egg &e) {
            return *e.crashQuestion != 0;
        });
        return rows;
    }();
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

void calmMoods(World &world) {
    for (auto &entry : world.moods) {
        entry.second = std::max(0, entry.second - 1);
    }
}

bool applyEgg(World &world, const Egg &egg, std::string &terminal) {
    if (*egg.crashQuestion) {
        // Wait for one key: y crashes the machine (System::input).
        world.crashQuestion = egg.crashQuestion;
        terminal += std::string(egg.crashQuestion) + " ";
        return true;
    }
    if (!egg.steps.empty()) {
        const int last = int(egg.steps.size()) - 1;
        int &count = world.moods[egg.group];
        terminal +=
            std::string(egg.steps[std::size_t(std::min(count, last))]) + "\n";
        if (count >= last) {
            world.rebootRequest = true;
        }
        ++count;
        return true;
    }
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
