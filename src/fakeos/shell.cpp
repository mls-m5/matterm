#include "shell.h"

#include "commands.h"
#include "eggs.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace fakeos {
namespace {

bool nameChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

std::string crlf(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
}

bool isOperatorStart(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

std::string escapeGlob(const std::string &text) {
    std::string out;
    for (char c : text) {
        if (c == '*' || c == '?' || c == '[' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

bool hasMeta(const std::string &pattern) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            ++i;
        }
        else if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[') {
            return true;
        }
    }
    return false;
}

} // namespace

bool Shell::parse(const std::string &line,
                  std::vector<Item> &items,
                  std::string &error) const {
    struct Token {
        enum class Kind { Word, Op } kind;
        Word word;
        std::string op;
    };
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '#') {
            break; // Comment to the end of the line.
        }
        if (isOperatorStart(c)) {
            std::string op(1, c);
            if (i + 1 < line.size() && line[i + 1] == c && c != ';' &&
                c != '<') {
                op += c; // ||  &&  >>
            }
            if (op == "&") {
                error = "matterm: background jobs are not supported";
                return false;
            }
            tokens.push_back({Token::Kind::Op, {}, op});
            i += op.size();
            continue;
        }
        Word word;
        Segment current;
        auto flush = [&]() {
            if (!current.text.empty() || current.quote) {
                word.segments.push_back(current);
            }
            current = Segment{};
        };
        while (i < line.size()) {
            const char d = line[i];
            if (d == ' ' || d == '\t' || isOperatorStart(d)) {
                break;
            }
            if (d == '\'') {
                flush();
                current.quote = '\'';
                const auto end = line.find('\'', i + 1);
                if (end == std::string::npos) {
                    error = "matterm: unexpected EOF while looking for "
                            "matching `''";
                    return false;
                }
                current.text = line.substr(i + 1, end - i - 1);
                i = end + 1;
                flush();
                // An empty '' still counts as a (empty) word segment.
                if (word.segments.empty() ||
                    word.segments.back().quote != '\'') {
                    word.segments.push_back({"", '\''});
                }
                continue;
            }
            if (d == '"') {
                flush();
                std::string text;
                ++i;
                bool closed = false;
                while (i < line.size()) {
                    if (line[i] == '"') {
                        closed = true;
                        ++i;
                        break;
                    }
                    if (line[i] == '\\' && i + 1 < line.size() &&
                        (line[i + 1] == '"' || line[i + 1] == '\\' ||
                         line[i + 1] == '$' || line[i + 1] == '`')) {
                        // A literal character: store it as its own quoted part.
                        word.segments.push_back({text, '"'});
                        text.clear();
                        word.segments.push_back(
                            {std::string(1, line[i + 1]), '\''});
                        i += 2;
                        continue;
                    }
                    text += line[i++];
                }
                if (!closed) {
                    error = "matterm: unexpected EOF while looking for "
                            "matching `\"'";
                    return false;
                }
                word.segments.push_back({text, '"'});
                continue;
            }
            if (d == '\\' && i + 1 < line.size()) {
                flush();
                word.segments.push_back({std::string(1, line[i + 1]), '\''});
                i += 2;
                continue;
            }
            current.text += d;
            ++i;
        }
        flush();
        tokens.push_back({Token::Kind::Word, std::move(word), {}});
    }

    // Build items: pipelines joined by ; && ||.
    Item item;
    Simple simple;
    Item::Join join = Item::Join::First;
    bool expectWord = false; // After a redirect operator.
    std::string pendingRedirect;
    auto closeSimple = [&](const std::string &next) -> bool {
        if (simple.words.empty() && !simple.hasIn && !simple.hasOut) {
            error =
                "matterm: syntax error near unexpected token `" + next + "'";
            return false;
        }
        item.pipeline.push_back(std::move(simple));
        simple = Simple{};
        return true;
    };
    for (std::size_t t = 0; t < tokens.size(); ++t) {
        const auto &token = tokens[t];
        if (token.kind == Token::Kind::Word) {
            if (expectWord) {
                if (pendingRedirect == "<") {
                    simple.hasIn = true;
                    simple.in = token.word;
                }
                else {
                    simple.hasOut = true;
                    simple.append = pendingRedirect == ">>";
                    simple.out = token.word;
                }
                expectWord = false;
            }
            else {
                simple.words.push_back(token.word);
            }
            continue;
        }
        const auto &op = token.op;
        if (expectWord) {
            error = "matterm: syntax error near unexpected token `" + op + "'";
            return false;
        }
        if (op == "<" || op == ">" || op == ">>") {
            expectWord = true;
            pendingRedirect = op;
        }
        else if (op == "|") {
            if (!closeSimple(op)) {
                return false;
            }
        }
        else { // ;  &&  ||
            if (simple.words.empty() && item.pipeline.empty() && op == ";" &&
                !simple.hasIn && !simple.hasOut) {
                error = "matterm: syntax error near unexpected token `;'";
                return false;
            }
            if (!closeSimple(op)) {
                return false;
            }
            item.join = join;
            items.push_back(std::move(item));
            item = Item{};
            join = op == ";"    ? Item::Join::Seq
                   : op == "&&" ? Item::Join::And
                                : Item::Join::Or;
        }
    }
    if (expectWord) {
        error = "matterm: syntax error near unexpected token `newline'";
        return false;
    }
    if (!simple.words.empty() || simple.hasIn || simple.hasOut) {
        item.pipeline.push_back(std::move(simple));
    }
    else if (!item.pipeline.empty() ||
             (join != Item::Join::First && join != Item::Join::Seq)) {
        error = "matterm: syntax error: unexpected end of line";
        return false;
    }
    if (!item.pipeline.empty()) {
        item.join = join;
        items.push_back(std::move(item));
    }
    return true;
}

std::string Shell::variable(const std::string &name) const {
    if (name == "?") {
        return std::to_string(world_.lastStatus);
    }
    const auto it = world_.env.find(name);
    return it == world_.env.end() ? std::string() : it->second;
}

// One expanded word (no glob, no splitting): for redirect targets.
std::string Shell::expandOne(const Word &word) const {
    const auto words = expand(word);
    std::string out;
    for (const auto &w : words) {
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

std::vector<std::string> Shell::expand(const Word &word) const {
    std::vector<std::string> results;
    std::string text, pattern;
    bool any = false;
    const bool wordQuoted =
        std::any_of(word.segments.begin(),
                    word.segments.end(),
                    [](const Segment &seg) { return seg.quote != 0; });
    auto finish = [&]() {
        if (!any || (text.empty() && !wordQuoted)) {
            any = false; // An unquoted empty expansion disappears.
            return;
        }
        if (hasMeta(pattern)) {
            // Match one level: the directory part is literal.
            const auto slash = pattern.rfind('/');
            const std::string dirPattern =
                slash == std::string::npos ? "" : pattern.substr(0, slash + 1);
            const std::string namePattern = slash == std::string::npos
                                                ? pattern
                                                : pattern.substr(slash + 1);
            std::string dirText;
            for (std::size_t i = 0; i < dirPattern.size(); ++i) {
                if (dirPattern[i] == '\\' && i + 1 < dirPattern.size()) {
                    ++i;
                }
                dirText += dirPattern[i];
            }
            std::vector<std::string> matches;
            if (!hasMeta(dirPattern)) {
                for (const auto &[name, node] : world_.fs.list(
                         world_.path(dirText.empty() ? "." : dirText))) {
                    (void)node;
                    if (name[0] == '.' && namePattern[0] != '.') {
                        continue;
                    }
                    if (globMatch(namePattern, name)) {
                        matches.push_back(dirText + name);
                    }
                }
            }
            if (!matches.empty()) {
                std::sort(matches.begin(), matches.end());
                for (auto &m : matches) {
                    results.push_back(std::move(m));
                }
                text.clear();
                pattern.clear();
                any = false;
                return;
            }
        }
        results.push_back(text);
        text.clear();
        pattern.clear();
        any = false;
    };
    for (std::size_t s = 0; s < word.segments.size(); ++s) {
        const auto &seg = word.segments[s];
        any = true;
        if (seg.quote == '\'') {
            text += seg.text;
            pattern += escapeGlob(seg.text);
            continue;
        }
        const bool quoted = seg.quote == '"';
        for (std::size_t i = 0; i < seg.text.size(); ++i) {
            const char c = seg.text[i];
            if (c == '~' && !quoted && s == 0 && i == 0 &&
                (seg.text.size() == 1 || seg.text[1] == '/')) {
                const auto home = world_.home();
                text += home;
                pattern += escapeGlob(home);
                continue;
            }
            if (c == '$' && i + 1 < seg.text.size()) {
                std::string name;
                std::size_t end = i + 1;
                if (seg.text[end] == '{') {
                    const auto close = seg.text.find('}', end);
                    if (close != std::string::npos) {
                        name = seg.text.substr(end + 1, close - end - 1);
                        end = close + 1;
                    }
                }
                else if (seg.text[end] == '?') {
                    name = "?";
                    ++end;
                }
                else {
                    while (end < seg.text.size() && nameChar(seg.text[end])) {
                        ++end;
                    }
                    name = seg.text.substr(i + 1, end - i - 1);
                }
                if (!name.empty()) {
                    const auto value = variable(name);
                    if (quoted) {
                        text += value;
                        pattern += escapeGlob(value);
                    }
                    else { // Unquoted: split on blanks.
                        for (char v : value) {
                            if (v == ' ' || v == '\t' || v == '\n') {
                                if (any && (!text.empty())) {
                                    finish();
                                    any = true;
                                }
                            }
                            else {
                                text += v;
                                pattern += escapeGlob(std::string(1, v));
                            }
                        }
                    }
                    i = end - 1;
                    continue;
                }
            }
            text += c;
            // Unquoted metacharacters stay active in the pattern.
            pattern +=
                quoted ? escapeGlob(std::string(1, c)) : std::string(1, c);
        }
    }
    finish();
    return results;
}

int Shell::runCommand(const std::vector<std::string> &args,
                      CommandIo &io,
                      std::string &terminal) {
    (void)terminal;
    const auto &name = args[0];
    if (const auto *c = findCommand(name); c && c->builtin) {
        return c->run(world_, args, io);
    }
    // Locate a file.
    std::string path;
    if (name.find('/') != std::string::npos) {
        path = world_.path(name);
        const auto *node = world_.fs.find(path);
        if (!node) {
            io.err += "matterm: " + name + ": No such file or directory\n";
            return 127;
        }
        if (node->directory) {
            io.err += "matterm: " + name + ": Is a directory\n";
            return 126;
        }
    }
    else {
        std::stringstream dirs(world_.env.count("PATH") ? world_.env["PATH"]
                                                        : "");
        std::string dir;
        while (std::getline(dirs, dir, ':')) {
            const auto candidate = VirtualFs::join(world_.path(dir), name);
            const auto *node = world_.fs.find(candidate);
            if (node && !node->directory && (node->mode & 0111)) {
                path = candidate;
                break;
            }
        }
        if (path.empty()) {
            io.err += "matterm: " + name + ": command not found\n";
            return 127;
        }
    }
    const auto *node = world_.fs.find(path);
    if (!(node->mode & 0111) || !world_.fs.canRead(*node, world_.user)) {
        io.err += "matterm: " + name + ": Permission denied\n";
        return 126;
    }
    static const std::string marker = "#!builtin ";
    if (node->data.compare(0, marker.size(), marker) != 0) {
        io.err += "matterm: " + name +
                  ": cannot execute binary file: Exec format error\n";
        return 126;
    }
    std::istringstream spec(
        node->data.substr(marker.size(),
                          node->data.find('\n') == std::string::npos
                              ? std::string::npos
                              : node->data.find('\n') - marker.size()));
    std::string target;
    spec >> target;
    std::vector<std::string> full{target};
    std::string extra;
    while (spec >> extra) {
        full.push_back(extra);
    }
    full.insert(full.end(), args.begin() + 1, args.end());
    if (const auto *c = findCommand(target)) {
        return c->run(world_, full, io);
    }
    world_.launch = World::Launch{
        target, std::vector<std::string>(full.begin() + 1, full.end())};
    return 0;
}

int Shell::runPipeline(const std::vector<Simple> &pipeline,
                       std::string &terminal) {
    std::string carried;
    int status = 0;
    for (std::size_t k = 0; k < pipeline.size(); ++k) {
        const auto &simple = pipeline[k];
        const bool last = k + 1 == pipeline.size();
        CommandIo io;
        io.in = carried;
        io.tty = last && !simple.hasOut;
        bool skip = false;
        if (simple.hasIn) {
            const auto file = expandOne(simple.in);
            const auto *node = world_.fs.find(world_.path(file));
            if (!node || node->directory ||
                !world_.fs.canRead(*node, world_.user)) {
                terminal += "matterm: " + file + ": " +
                            (!node             ? "No such file or directory"
                             : node->directory ? "Is a directory"
                                               : "Permission denied") +
                            "\n";
                status = 1;
                skip = true;
            }
            else {
                io.in = node->data;
            }
        }
        std::vector<std::string> args;
        for (const auto &word : simple.words) {
            for (auto &a : expand(word)) {
                args.push_back(std::move(a));
            }
        }
        if (!skip) {
            // NAME=value alone sets a variable.
            if (!args.empty() &&
                std::all_of(args.begin(), args.end(), [](const std::string &a) {
                    const auto eq = a.find('=');
                    return eq != std::string::npos && eq > 0 &&
                           !std::isdigit((unsigned char)a[0]) &&
                           std::all_of(
                               a.begin(), a.begin() + long(eq), nameChar);
                })) {
                for (const auto &a : args) {
                    world_.env[a.substr(0, a.find('='))] =
                        a.substr(a.find('=') + 1);
                }
                status = 0;
            }
            else if (!args.empty()) {
                status = runCommand(args, io, terminal);
            }
        }
        terminal += io.err;
        if (simple.hasOut && !skip) {
            const auto file = expandOne(simple.out);
            const auto e = world_.fs.write(world_.path(file),
                                           io.out,
                                           world_.user,
                                           world_.unixNow(),
                                           simple.append);
            if (e != VirtualFs::Error::None) {
                terminal +=
                    "matterm: " + file + ": " + VirtualFs::message(e) + "\n";
                status = 1;
            }
        }
        else if (last) {
            terminal += io.out;
        }
        carried = io.out;
        if (world_.exitRequest || world_.resetRequest) {
            break;
        }
    }
    return status;
}

std::string Shell::run(const std::string &line) {
    std::string terminal;
    // Easter eggs see the line first.
    if (const auto *egg = matchEgg(world_, line)) {
        if (applyEgg(world_, *egg, terminal) && egg->replaces) {
            return crlf(terminal);
        }
    }
    std::vector<Item> items;
    std::string error;
    if (!parse(line, items, error)) {
        world_.lastStatus = 2;
        return crlf(error + "\n");
    }
    for (const auto &item : items) {
        const bool run =
            item.join == Item::Join::First || item.join == Item::Join::Seq ||
            (item.join == Item::Join::And && world_.lastStatus == 0) ||
            (item.join == Item::Join::Or && world_.lastStatus != 0);
        if (!run) {
            continue;
        }
        world_.lastStatus = runPipeline(item.pipeline, terminal);
        if (world_.exitRequest || world_.resetRequest || world_.launch) {
            break;
        }
    }
    return crlf(terminal);
}

Shell::Completion Shell::complete(const std::string &prefix) const {
    Completion result;
    std::size_t start = prefix.size();
    while (start > 0 && prefix[start - 1] != ' ' && prefix[start - 1] != '\t' &&
           !isOperatorStart(prefix[start - 1])) {
        --start;
    }
    result.start = start;
    const auto word = prefix.substr(start);
    // Command position: nothing but blanks since the line start or an operator.
    std::size_t before = start;
    while (before > 0 &&
           (prefix[before - 1] == ' ' || prefix[before - 1] == '\t')) {
        --before;
    }
    const bool commandPosition =
        before == 0 || isOperatorStart(prefix[before - 1]);
    std::set<std::string> options;
    if (commandPosition && word.find('/') == std::string::npos) {
        for (const auto &c : commands()) {
            if (c.builtin &&
                std::string(c.name).compare(0, word.size(), word) == 0) {
                options.insert(c.name);
            }
        }
        std::stringstream dirs(world_.env.count("PATH") ? world_.env.at("PATH")
                                                        : "");
        std::string dir;
        while (std::getline(dirs, dir, ':')) {
            for (const auto &[name, node] : world_.fs.list(world_.path(dir))) {
                if (!node->directory && (node->mode & 0111) &&
                    name.compare(0, word.size(), word) == 0) {
                    options.insert(name);
                }
            }
        }
    }
    else {
        const auto slash = word.rfind('/');
        const std::string dirPart =
            slash == std::string::npos ? "" : word.substr(0, slash + 1);
        const std::string namePart =
            slash == std::string::npos ? word : word.substr(slash + 1);
        std::string dirText = dirPart;
        if (dirText.compare(0, 2, "~/") == 0) {
            dirText = world_.home() + dirText.substr(1);
        }
        for (const auto &[name, node] :
             world_.fs.list(world_.path(dirText.empty() ? "." : dirText))) {
            if (name.compare(0, namePart.size(), namePart) != 0 ||
                (name[0] == '.' && namePart.empty())) {
                continue;
            }
            options.insert(dirPart + name + (node->directory ? "/" : ""));
        }
    }
    result.options.assign(options.begin(), options.end());
    return result;
}

} // namespace fakeos
