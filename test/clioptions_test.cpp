// The option tables behind `matterm --completion` against what the programs really parse and print.
#include "clioptions.h"
#include "mls-unit-test/unittest.h"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace frontend::cli;

namespace {
std::string slurp(const std::string &relative) {
    std::ifstream in(std::string(MATTERM_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
const Program &program(const std::string &name) {
    for (const auto &p : programs()) {
        if (name == p.name) return p;
    }
    static const Program none{"", "", {}};
    return none;
}
// Every "--flag" that the code compares an argument with.
std::set<std::string> parsedFlags(const std::string &source) {
    std::set<std::string> out;
    const std::regex compare("==\\s*\"(--[a-z][a-z-]*)\"");
    for (std::sregex_iterator it(source.begin(), source.end(), compare), end; it != end; ++it) {
        out.insert((*it)[1]);
    }
    return out;
}
std::set<std::string> tableFlags(const Program &p) {
    std::set<std::string> out;
    for (const auto &o : p.options) out.insert(o.name);
    return out;
}
// Flags written in lines of the source that are not comparisons: the usage text.
std::set<std::string> mentionedFlags(const std::string &source) {
    std::set<std::string> out;
    std::istringstream lines(source);
    std::string line;
    const std::regex flag("--[a-z][a-z-]*");
    while (std::getline(lines, line)) {
        if (line.find("==") != std::string::npos) continue;
        for (std::sregex_iterator it(line.begin(), line.end(), flag), end; it != end; ++it) out.insert(it->str());
    }
    return out;
}
std::string missing(const std::set<std::string> &want, const std::set<std::string> &have) {
    std::string m;
    for (const auto &w : want) {
        if (have.count(w) == 0) m += w + " ";
    }
    return m;
}
} // namespace

TEST_SUIT_BEGIN(CliOptions)

TEST_CASE("The tables are the options the programs parse") {
    const std::pair<const char *, const char *> sources[] = {
        {"matterm", "app/main.cpp"}, {"matterm-server", "src/server/config.cpp"}, {"matterm-daemon", "src/daemon/main.cpp"}};
    for (const auto &[name, file] : sources) {
        const auto parsed = parsedFlags(slurp(file));
        const auto table = tableFlags(program(name));
        EXPECT(!parsed.empty());
        EXPECT_EQ(missing(parsed, table), std::string()); // parsed but not in the table
        EXPECT_EQ(missing(table, parsed), std::string()); // in the table but not parsed
    }
}

TEST_CASE("Every completed option is in the program's help text") {
    // matterm and the daemon print their usage from their main file; the server from its main.cpp.
    const std::pair<const char *, const char *> helps[] = {
        {"matterm", "app/main.cpp"}, {"matterm-server", "src/server/main.cpp"}, {"matterm-daemon", "src/daemon/main.cpp"}};
    for (const auto &[name, file] : helps) {
        const auto mentioned = mentionedFlags(slurp(file));
        std::set<std::string> shown;
        for (const auto &o : program(name).options) {
            if (!o.hidden && std::string(o.name) != "--help") shown.insert(o.name);
        }
        EXPECT_EQ(missing(shown, mentioned), std::string());
    }
}

TEST_CASE("The scripts offer every visible option and never a hidden one") {
    for (const char *shell : {"bash", "zsh", "fish"}) {
        const auto script = completionScript(shell);
        EXPECT(!script.empty());
        for (const auto &p : programs()) {
            EXPECT(script.find(p.name) != std::string::npos);
            for (const auto &o : p.options) {
                const std::string flag = std::string(shell) == "fish" ? std::string(o.name + 2) : std::string(o.name);
                const bool present = script.find(flag) != std::string::npos;
                EXPECT(o.hidden ? !present || std::string(shell) == "fish" : present);
            }
        }
        EXPECT(script.find("pty-tabs-smoke-test") == std::string::npos);
        EXPECT(script.find("--daemon-host") != std::string::npos || std::string(shell) == "fish");
        EXPECT(script.find(".ssh/config") != std::string::npos); // Hosts come from ssh's config only.
        EXPECT(script.find("known_hosts") == std::string::npos);
    }
    EXPECT(completionScript("csh").empty());
}

TEST_CASE("The scripts complete values by kind: choices, files, folders, hosts") {
    const auto bash = completionScript("bash");
    EXPECT(bash.find("--term)") != std::string::npos);
    EXPECT(bash.find("xterm-256color") != std::string::npos);
    EXPECT(bash.find("compgen -d") != std::string::npos);        // --static-dir
    EXPECT(bash.find("_matterm_hosts") != std::string::npos);    // --daemon-host
    EXPECT(bash.find("complete -o filenames -F _matterm matterm") != std::string::npos);
    EXPECT(bash.find("complete -o filenames -F _matterm_server matterm-server") != std::string::npos);
    const auto zsh = completionScript("zsh");
    EXPECT(zsh.rfind("#compdef matterm matterm-server matterm-daemon", 0) == 0);
    EXPECT(zsh.find("--completion[") != std::string::npos);
    EXPECT(zsh.find(":value:(bash zsh fish)") != std::string::npos);
    const auto fish = completionScript("fish");
    EXPECT(fish.find("complete -c matterm -l completion -x -a 'bash zsh fish'") != std::string::npos);
    EXPECT(fish.find("complete -c matterm -l help -s h") != std::string::npos);
    EXPECT(fish.find("complete -c matterm-server -l static-dir -r") != std::string::npos);
}

TEST_CASE("Help texts never break the quoting of the scripts") {
    for (const auto &p : programs()) {
        for (const auto &o : p.options) {
            const std::string help = o.help;
            EXPECT(help.find('\'') == std::string::npos && help.find('"') == std::string::npos);
            EXPECT(help.find('\n') == std::string::npos);
        }
    }
    // The generators drop the characters that matter inside quotes and brackets.
    EXPECT_EQ(detail::clean("a'b[c]:d$e"), std::string("a b c  d e"));
}

TEST_SUIT_END
