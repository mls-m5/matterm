#include "commands.h"

#include "programs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <regex>
#include <sstream>

namespace fakeos {
namespace {

using Args = std::vector<std::string>;

// ---- helpers ---------------------------------------------------------------

std::string base(const std::string &path) {
    return VirtualFs::nameOf(path);
}

// Splits "-abc" style flags from operands. Stops at "--". `flags` receives the
// letters; long options are kept as "--name" entries in `longs`.
struct Options {
    std::string flags;
    std::vector<std::string> longs;
    Args operands;
    std::string bad; // First unknown option, empty when all were known.
    bool has(char c) const {
        return flags.find(c) != std::string::npos;
    }
    bool hasLong(const std::string &name) const {
        return std::find(longs.begin(), longs.end(), name) != longs.end();
    }
};

Options parse(const Args &args, const std::string &known) {
    Options o;
    bool options = true;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto &a = args[i];
        if (options && a == "--") {
            options = false;
        }
        else if (options && a.size() > 2 && a[0] == '-' && a[1] == '-') {
            o.longs.push_back(a);
        }
        else if (options && a.size() > 1 && a[0] == '-' &&
                 !(a[1] >= '0' && a[1] <= '9')) {
            for (std::size_t k = 1; k < a.size(); ++k) {
                if (known.find(a[k]) == std::string::npos && o.bad.empty()) {
                    o.bad = std::string(1, a[k]);
                }
                o.flags.push_back(a[k]);
            }
        }
        else {
            o.operands.push_back(a);
        }
    }
    return o;
}

int badOption(const Args &args, const Options &o, CommandIo &io) {
    io.err += args[0] + ": invalid option -- '" + o.bad + "'\n";
    return 2;
}

std::string fail(const std::string &cmd,
                 const std::string &name,
                 VirtualFs::Error e) {
    return cmd + ": " + name + ": " + VirtualFs::message(e) + "\n";
}

// Reads a file for `user`; returns false and appends an error to io.err.
bool readFile(World &w,
              const std::string &cmd,
              const std::string &name,
              std::string &out,
              CommandIo &io) {
    const auto *node = w.fs.find(w.path(name));
    if (!node) {
        io.err += fail(cmd, name, VirtualFs::Error::NotFound);
        return false;
    }
    if (node->directory) {
        io.err += fail(cmd, name, VirtualFs::Error::IsDirectory);
        return false;
    }
    if (!w.fs.canRead(*node, w.user)) {
        io.err += fail(cmd, name, VirtualFs::Error::Permission);
        return false;
    }
    out = node->data;
    return true;
}

// Input text: the operands' files concatenated, or stdin when there are none.
bool inputOf(World &w,
             const std::string &cmd,
             const Args &files,
             std::string &text,
             CommandIo &io) {
    if (files.empty()) {
        text = io.in;
        return true;
    }
    bool ok = true;
    for (const auto &name : files) {
        std::string part;
        if (name == "-") {
            text += io.in;
        }
        else if (readFile(w, cmd, name, part, io)) {
            text += part;
        }
        else {
            ok = false;
        }
    }
    return ok;
}

int toInt(const std::string &s, int fallback) {
    if (s.empty()) {
        return fallback;
    }
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return *end ? fallback : int(v);
}

const char *kMonths[] = {"Jan",
                         "Feb",
                         "Mar",
                         "Apr",
                         "May",
                         "Jun",
                         "Jul",
                         "Aug",
                         "Sep",
                         "Oct",
                         "Nov",
                         "Dec"};
const char *kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};

struct Civil {
    int year, month, day, hour, minute, second, weekday; // weekday 0 = Thursday
};
Civil civil(std::int64_t t) {
    std::int64_t days = t / 86400;
    std::int64_t rest = t % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    Civil c{};
    c.hour = int(rest / 3600);
    c.minute = int(rest % 3600 / 60);
    c.second = int(rest % 60);
    c.weekday = int(((days % 7) + 7) % 7);
    // Howard Hinnant's civil_from_days.
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = unsigned(z - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = std::int64_t(yoe) + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    c.day = int(doy - (153 * mp + 2) / 5 + 1);
    c.month = int(mp < 10 ? mp + 3 : mp - 9);
    c.year = int(y + (c.month <= 2));
    return c;
}

std::string two(int v) {
    return (v < 10 ? "0" : "") + std::to_string(v);
}

// ---- file and text commands ----------------------------------------------

int cmdEcho(World &, const Args &args, CommandIo &io) {
    bool newline = true, escapes = false;
    std::size_t first = 1;
    for (; first < args.size(); ++first) {
        if (args[first] == "-n") {
            newline = false;
        }
        else if (args[first] == "-e") {
            escapes = true;
        }
        else if (args[first] == "-ne" || args[first] == "-en") {
            newline = false;
            escapes = true;
        }
        else {
            break;
        }
    }
    std::string text;
    for (std::size_t k = first; k < args.size(); ++k) {
        text += (k > first ? " " : "") + args[k];
    }
    if (escapes) {
        std::string out;
        for (std::size_t k = 0; k < text.size(); ++k) {
            if (text[k] == '\\' && k + 1 < text.size()) {
                switch (text[++k]) {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'e':
                    out += '\x1b';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '0':
                    // \033 style octal.
                    {
                        int v = 0, digits = 0;
                        while (digits < 3 && k < text.size() &&
                               text[k] >= '0' && text[k] <= '7') {
                            v = v * 8 + (text[k] - '0');
                            ++k;
                            ++digits;
                        }
                        --k;
                        out += char(v);
                    }
                    break;
                default:
                    out += '\\';
                    out += text[k];
                    break;
                }
            }
            else {
                out += text[k];
            }
        }
        text = out;
    }
    io.out += text + (newline ? "\n" : "");
    return 0;
}

int cmdPwd(World &w, const Args &, CommandIo &io) {
    io.out += w.cwd + "\n";
    return 0;
}

int cmdCd(World &w, const Args &args, CommandIo &io) {
    std::string target = args.size() > 1 ? args[1] : w.home();
    if (target == "-") {
        target = w.env.count("OLDPWD") ? w.env["OLDPWD"] : w.cwd;
        io.out += target + "\n";
    }
    const auto path = w.path(target);
    const auto *node = w.fs.find(path);
    if (!node) {
        io.err += "matterm: cd: " + target + ": No such file or directory\n";
        return 1;
    }
    if (!node->directory) {
        io.err += "matterm: cd: " + target + ": Not a directory\n";
        return 1;
    }
    if (!w.fs.canSearch(*node, w.user)) {
        io.err += "matterm: cd: " + target + ": Permission denied\n";
        return 1;
    }
    w.env["OLDPWD"] = w.cwd;
    w.cwd = path;
    w.env["PWD"] = path;
    return 0;
}

std::string modeString(const VirtualFs::Node &n) {
    std::string s(1, n.directory ? 'd' : '-');
    const char *bits = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        s += (n.mode & (0400 >> i)) ? bits[i] : '-';
    }
    return s;
}

struct LsEntry {
    std::string name;
    const VirtualFs::Node *node;
};

std::string colored(const LsEntry &e, bool tty) {
    if (!tty) {
        return e.name;
    }
    if (e.node->directory) {
        return "\x1b[1;34m" + e.name + "\x1b[0m";
    }
    if (e.node->mode & 0111) {
        return "\x1b[1;32m" + e.name + "\x1b[0m";
    }
    return e.name;
}

bool nameLess(const std::string &a, const std::string &b) {
    // Like a UTF-8 locale: ignore case and leading dots first.
    auto key = [](const std::string &s) {
        std::string k;
        for (char c : s) {
            if (c != '.') {
                k += char(std::tolower((unsigned char)c));
            }
        }
        return k;
    };
    const auto ka = key(a), kb = key(b);
    return ka != kb ? ka < kb : a < b;
}

void listEntries(World &w,
                 std::vector<LsEntry> entries,
                 const Options &o,
                 CommandIo &io,
                 const World &now) {
    std::sort(
        entries.begin(), entries.end(), [](const LsEntry &a, const LsEntry &b) {
            return nameLess(a.name, b.name);
        });
    if (o.has('l')) {
        std::size_t blocks = 0;
        for (const auto &e : entries) {
            blocks +=
                e.node->directory ? 4 : (e.node->data.size() + 1023) / 1024 * 1;
        }
        io.out += "total " + std::to_string(blocks) + "\n";
        std::size_t sizeWidth = 1;
        for (const auto &e : entries) {
            const auto size = e.node->directory ? 4096 : e.node->data.size();
            sizeWidth = std::max(sizeWidth, std::to_string(size).size());
        }
        for (const auto &e : entries) {
            const auto size = e.node->directory ? 4096 : e.node->data.size();
            std::string sz = std::to_string(size);
            if (o.has('h') && size >= 1024) {
                const double k = double(size) / 1024;
                const int whole = int(k * 10 + 0.5);
                sz = k < 10 ? std::to_string(whole / 10) + "." +
                                  std::to_string(whole % 10) + "K"
                            : std::to_string(int(k + 0.5)) + "K";
            }
            const auto c = civil(e.node->mtime);
            std::string date = std::string(kMonths[c.month - 1]) + " " +
                               (c.day < 10 ? " " : "") + std::to_string(c.day) +
                               " " + two(c.hour) + ":" + two(c.minute);
            std::size_t links = 1;
            if (e.node->directory) {
                links = 2;
                for (const auto &child : e.node->children) {
                    links += child.second->directory;
                }
            }
            io.out +=
                modeString(*e.node) + " " + std::to_string(links) + " " +
                e.node->owner + " " + e.node->owner + " " +
                std::string(sizeWidth > sz.size() ? sizeWidth - sz.size() : 0,
                            ' ') +
                sz + " " + date + " " + colored(e, io.tty) + "\n";
        }
        return;
    }
    if (o.has('1') || !io.tty || entries.empty()) {
        for (const auto &e : entries) {
            io.out += colored(e, io.tty) + "\n";
        }
        return;
    }
    // Columns, filled down like ls does.
    std::size_t widest = 0;
    for (const auto &e : entries) {
        widest = std::max(widest, e.name.size());
    }
    const std::size_t columnWidth = widest + 2;
    const std::size_t width = std::size_t(std::max(20, now.columns));
    const std::size_t perRow = std::max<std::size_t>(1, width / columnWidth);
    const std::size_t rows = (entries.size() + perRow - 1) / perRow;
    const std::size_t columns = (entries.size() + rows - 1) / rows;
    for (std::size_t r = 0; r < rows; ++r) {
        std::string line;
        for (std::size_t c = 0; c < columns; ++c) {
            const auto index = c * rows + r;
            if (index >= entries.size()) {
                break;
            }
            const auto &e = entries[index];
            line += colored(e, true);
            if (index + rows < entries.size()) {
                line += std::string(columnWidth - e.name.size(), ' ');
            }
        }
        io.out += line + "\n";
    }
    (void)w;
}

int cmdLs(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "laA1hd");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    int status = 0;
    Args targets = o.operands;
    if (targets.empty()) {
        targets.push_back(".");
    }
    std::vector<LsEntry> files;
    std::vector<std::pair<std::string, const VirtualFs::Node *>> dirs;
    for (const auto &t : targets) {
        const auto path = w.path(t);
        const auto *node = w.fs.find(path);
        if (!node) {
            io.err +=
                "ls: cannot access '" + t + "': No such file or directory\n";
            status = 2;
        }
        else if (node->directory && !o.has('d')) {
            dirs.emplace_back(t, node);
        }
        else {
            files.push_back({t, node});
        }
    }
    bool printed = false;
    if (!files.empty()) {
        listEntries(w, files, o, io, w);
        printed = true;
    }
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        const auto &[name, node] = dirs[i];
        if (!w.fs.canRead(*node, w.user)) {
            io.err +=
                "ls: cannot open directory '" + name + "': Permission denied\n";
            status = 2;
            continue;
        }
        if (printed) {
            io.out += "\n";
        }
        if (targets.size() > 1) {
            io.out += name + ":\n";
        }
        std::vector<LsEntry> entries;
        if (o.has('a')) {
            entries.push_back({".", node});
            entries.push_back({"..", node});
        }
        for (const auto &[child, n] : node->children) {
            if (child[0] != '.' || o.has('a') || o.has('A')) {
                entries.push_back({child, n.get()});
            }
        }
        listEntries(w, entries, o, io, w);
        printed = true;
    }
    return status;
}

int cmdCat(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "n");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    std::string text;
    const bool ok = inputOf(w, "cat", o.operands, text, io);
    if (o.has('n')) {
        int n = 1;
        for (const auto &line : splitLines(text)) {
            std::string num = std::to_string(n++);
            io.out +=
                std::string(6 - std::min<std::size_t>(5, num.size()), ' ') +
                num + "\t" + line + "\n";
        }
    }
    else {
        io.out += text;
    }
    return ok ? 0 : 1;
}

int cmdMkdir(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "p");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    if (o.operands.empty()) {
        io.err += "mkdir: missing operand\n";
        return 1;
    }
    int status = 0;
    for (const auto &name : o.operands) {
        const auto e =
            w.fs.mkdir(w.path(name), w.user, w.unixNow(), o.has('p'));
        if (e != VirtualFs::Error::None &&
            !(o.has('p') && e == VirtualFs::Error::Exists)) {
            io.err += "mkdir: cannot create directory '" + name +
                      "': " + VirtualFs::message(e) + "\n";
            status = 1;
        }
    }
    return status;
}

int cmdTouch(World &w, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "touch: missing file operand\n";
        return 1;
    }
    int status = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto e = w.fs.touch(w.path(args[i]), w.user, w.unixNow());
        if (e != VirtualFs::Error::None) {
            io.err += "touch: cannot touch '" + args[i] +
                      "': " + VirtualFs::message(e) + "\n";
            status = 1;
        }
    }
    return status;
}

int cmdRm(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "rRfv");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    if (o.operands.empty()) {
        if (!o.has('f')) {
            io.err += "rm: missing operand\n";
            return 1;
        }
        return 0;
    }
    const bool recursive = o.has('r') || o.has('R');
    int status = 0;
    for (const auto &name : o.operands) {
        const auto path = w.path(name);
        if (path == "/" && recursive) {
            io.err += "rm: it is dangerous to operate recursively on '/'\n"
                      "rm: use --no-preserve-root to override this failsafe\n";
            status = 1;
            continue;
        }
        const auto *node = w.fs.find(path);
        if (!node) {
            if (!o.has('f')) {
                io.err += "rm: cannot remove '" + name +
                          "': No such file or directory\n";
                status = 1;
            }
            continue;
        }
        if (node->directory && !recursive) {
            io.err += "rm: cannot remove '" + name + "': Is a directory\n";
            status = 1;
            continue;
        }
        const auto e = w.fs.remove(path, w.user, recursive);
        if (e != VirtualFs::Error::None) {
            io.err += "rm: cannot remove '" + name +
                      "': " + VirtualFs::message(e) + "\n";
            status = 1;
        }
        else if (o.has('v')) {
            io.out += "removed '" + name + "'\n";
        }
        // Standing inside a removed directory leaves the shell in a ghost cwd,
        // as in a real system; cd out works.
    }
    return status;
}

// cp and mv: the last operand is the target; into a directory keeps the name.
int copyOrMove(World &w, const Args &args, CommandIo &io, bool move) {
    const auto o = parse(args, "rRf");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    const std::string cmd = move ? "mv" : "cp";
    if (o.operands.size() < 2) {
        io.err += cmd + ": missing file operand\n";
        return 1;
    }
    const auto &target = o.operands.back();
    const auto targetPath = w.path(target);
    const auto *targetNode = w.fs.find(targetPath);
    const bool intoDir = targetNode && targetNode->directory;
    if (o.operands.size() > 2 && !intoDir) {
        io.err += cmd + ": target '" + target + "' is not a directory\n";
        return 1;
    }
    int status = 0;
    for (std::size_t i = 0; i + 1 < o.operands.size(); ++i) {
        const auto &name = o.operands[i];
        const auto from = w.path(name);
        const auto to =
            intoDir ? VirtualFs::join(targetPath, base(from)) : targetPath;
        const auto e =
            move ? w.fs.move(from, to, w.user, w.unixNow())
                 : w.fs.copy(
                       from, to, w.user, w.unixNow(), o.has('r') || o.has('R'));
        if (e == VirtualFs::Error::IsDirectory && !move) {
            io.err +=
                "cp: -r not specified; omitting directory '" + name + "'\n";
            status = 1;
        }
        else if (e != VirtualFs::Error::None) {
            io.err += cmd + ": cannot " + (move ? "move" : "copy") + " '" +
                      name + "' to '" + target + "': " + VirtualFs::message(e) +
                      "\n";
            status = 1;
        }
    }
    return status;
}
int cmdCp(World &w, const Args &args, CommandIo &io) {
    return copyOrMove(w, args, io, false);
}
int cmdMv(World &w, const Args &args, CommandIo &io) {
    return copyOrMove(w, args, io, true);
}

int headTail(World &w, const Args &args, CommandIo &io, bool head) {
    const std::string cmd = head ? "head" : "tail";
    int count = 10;
    Args files;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-n" && i + 1 < args.size()) {
            count = toInt(args[++i], -1);
        }
        else if (args[i].size() > 2 && args[i].compare(0, 2, "-n") == 0) {
            count = toInt(args[i].substr(2), -1);
        }
        else if (args[i].size() > 1 && args[i][0] == '-' &&
                 std::isdigit((unsigned char)args[i][1])) {
            count = toInt(args[i].substr(1), -1);
        }
        else {
            files.push_back(args[i]);
        }
    }
    if (count < 0) {
        io.err += cmd + ": invalid number of lines\n";
        return 1;
    }
    int status = 0;
    const bool headers = files.size() > 1;
    auto emit = [&](const std::string &text) {
        const auto lines = splitLines(text);
        const auto n = std::size_t(count);
        const auto begin = head ? 0 : (lines.size() > n ? lines.size() - n : 0);
        const auto end = head ? std::min(n, lines.size()) : lines.size();
        for (auto i = begin; i < end; ++i) {
            io.out += lines[i] + "\n";
        }
    };
    if (files.empty()) {
        emit(io.in);
        return 0;
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string text;
        if (!readFile(w, cmd, files[i], text, io)) {
            status = 1;
            continue;
        }
        if (headers) {
            io.out +=
                (i ? "\n" : "") + std::string("==> ") + files[i] + " <==\n";
        }
        emit(text);
    }
    return status;
}
int cmdHead(World &w, const Args &args, CommandIo &io) {
    return headTail(w, args, io, true);
}
int cmdTail(World &w, const Args &args, CommandIo &io) {
    return headTail(w, args, io, false);
}

int cmdWc(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "lwc");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    const bool all = !o.has('l') && !o.has('w') && !o.has('c');
    int status = 0;
    std::size_t tl = 0, tw = 0, tc = 0;
    auto count = [&](const std::string &text, const std::string &label) {
        std::size_t l = std::count(text.begin(), text.end(), '\n'), words = 0,
                    c = text.size();
        bool in = false;
        for (char ch : text) {
            const bool space = std::isspace((unsigned char)ch);
            words += (!space && !in);
            in = !space;
        }
        tl += l;
        tw += words;
        tc += c;
        std::string line;
        if (all || o.has('l'))
            line += std::string(
                        8 - std::min<std::size_t>(7, std::to_string(l).size()),
                        ' ') +
                    std::to_string(l);
        if (all || o.has('w'))
            line += std::string(8 - std::min<std::size_t>(
                                        7, std::to_string(words).size()),
                                ' ') +
                    std::to_string(words);
        if (all || o.has('c'))
            line += std::string(
                        8 - std::min<std::size_t>(7, std::to_string(c).size()),
                        ' ') +
                    std::to_string(c);
        io.out += line + (label.empty() ? "" : " " + label) + "\n";
    };
    if (o.operands.empty()) {
        count(io.in, "");
        return 0;
    }
    for (const auto &name : o.operands) {
        std::string text;
        if (readFile(w, "wc", name, text, io)) {
            count(text, name);
        }
        else {
            status = 1;
        }
    }
    if (o.operands.size() > 1) {
        std::string line;
        if (all || o.has('l'))
            line += std::string(
                        8 - std::min<std::size_t>(7, std::to_string(tl).size()),
                        ' ') +
                    std::to_string(tl);
        if (all || o.has('w'))
            line += std::string(
                        8 - std::min<std::size_t>(7, std::to_string(tw).size()),
                        ' ') +
                    std::to_string(tw);
        if (all || o.has('c'))
            line += std::string(
                        8 - std::min<std::size_t>(7, std::to_string(tc).size()),
                        ' ') +
                    std::to_string(tc);
        io.out += line + " total\n";
    }
    return status;
}

int cmdGrep(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "ivncFEr");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    if (o.operands.empty()) {
        io.err += "Usage: grep [OPTION]... PATTERN [FILE]...\n";
        return 2;
    }
    const auto pattern = o.operands[0];
    Args files(o.operands.begin() + 1, o.operands.end());
    std::regex re;
    if (!o.has('F')) {
        try {
            re = std::regex(pattern,
                            o.has('i')
                                ? std::regex::icase | std::regex::ECMAScript
                                : std::regex::ECMAScript);
        }
        catch (const std::regex_error &) {
            io.err += "grep: Invalid regular expression\n";
            return 2;
        }
    }
    auto lower = [](std::string s) {
        for (auto &c : s)
            c = char(std::tolower((unsigned char)c));
        return s;
    };
    auto matches = [&](const std::string &line) {
        if (o.has('F')) {
            return o.has('i')
                       ? lower(line).find(lower(pattern)) != std::string::npos
                       : line.find(pattern) != std::string::npos;
        }
        return std::regex_search(line, re);
    };
    int status = 1;
    const bool names = files.size() > 1;
    auto scan = [&](const std::string &text, const std::string &label) {
        std::size_t hits = 0, n = 0;
        for (const auto &line : splitLines(text)) {
            ++n;
            if (matches(line) != o.has('v')) {
                ++hits;
                status = 0;
                if (!o.has('c')) {
                    io.out += (names ? label + ":" : "") +
                              (o.has('n') ? std::to_string(n) + ":" : "") +
                              line + "\n";
                }
            }
        }
        if (o.has('c')) {
            io.out += (names ? label + ":" : "") + std::to_string(hits) + "\n";
        }
    };
    if (files.empty()) {
        scan(io.in, "(standard input)");
        return status;
    }
    for (const auto &name : files) {
        std::string text;
        if (readFile(w, "grep", name, text, io)) {
            scan(text, name);
        }
        else {
            status = 2;
        }
    }
    return status;
}

int cmdSort(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "rnu");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    std::string text;
    const bool ok = inputOf(w, "sort", o.operands, text, io);
    auto lines = splitLines(text);
    auto number = [](const std::string &s) {
        return std::strtod(s.c_str(), nullptr);
    };
    std::stable_sort(lines.begin(),
                     lines.end(),
                     [&](const std::string &a, const std::string &b) {
                         if (o.has('n')) {
                             const auto na = number(a), nb = number(b);
                             if (na != nb)
                                 return o.has('r') ? na > nb : na < nb;
                         }
                         return o.has('r') ? a > b : a < b;
                     });
    if (o.has('u')) {
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    }
    for (const auto &l : lines) {
        io.out += l + "\n";
    }
    return ok ? 0 : 2;
}

int cmdUniq(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "c");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    std::string text;
    const bool ok = inputOf(w, "uniq", o.operands, text, io);
    const auto lines = splitLines(text);
    for (std::size_t i = 0; i < lines.size();) {
        std::size_t j = i;
        while (j < lines.size() && lines[j] == lines[i]) {
            ++j;
        }
        if (o.has('c')) {
            const auto n = std::to_string(j - i);
            io.out += std::string(7 - std::min<std::size_t>(6, n.size()), ' ') +
                      n + " ";
        }
        io.out += lines[i] + "\n";
        i = j;
    }
    return ok ? 0 : 1;
}

int cmdSeq(World &, const Args &args, CommandIo &io) {
    int first = 1, step = 1, last = 0;
    if (args.size() == 2) {
        last = toInt(args[1], 0);
    }
    else if (args.size() == 3) {
        first = toInt(args[1], 1);
        last = toInt(args[2], 0);
    }
    else if (args.size() == 4) {
        first = toInt(args[1], 1);
        step = toInt(args[2], 1);
        last = toInt(args[3], 0);
    }
    else {
        io.err += "seq: missing operand\n";
        return 1;
    }
    if (step == 0 || (step > 0 && last - first > 100000)) {
        io.err += "seq: invalid increment or range\n";
        return 1;
    }
    for (int v = first; step > 0 ? v <= last : v >= last; v += step) {
        io.out += std::to_string(v) + "\n";
    }
    return 0;
}

int cmdYes(World &, const Args &args, CommandIo &io) {
    std::string word = "y";
    if (args.size() > 1) {
        word.clear();
        for (std::size_t i = 1; i < args.size(); ++i) {
            word += (i > 1 ? " " : "") + args[i];
        }
    }
    // The real command never ends; a pretend one stops at a screenful of
    // scrollback, which is what `yes | head` needs.
    for (int i = 0; i < 2000; ++i) {
        io.out += word + "\n";
    }
    return 0;
}

int cmdFind(World &w, const Args &args, CommandIo &io) {
    std::string start = ".", name, type;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-name" && i + 1 < args.size()) {
            name = args[++i];
        }
        else if (args[i] == "-type" && i + 1 < args.size()) {
            type = args[++i];
        }
        else if (args[i][0] != '-') {
            start = args[i];
        }
    }
    const auto root = w.path(start);
    if (!w.fs.find(root)) {
        io.err += "find: '" + start + "': No such file or directory\n";
        return 1;
    }
    std::size_t emitted = 0;
    std::vector<std::pair<std::string, std::string>> stack{{root, start}};
    std::vector<std::string> results;
    // Depth-first in name order.
    std::function<void(const std::string &, const std::string &)> walk =
        [&](const std::string &abs, const std::string &shown) {
            const auto *node = w.fs.find(abs);
            if (!node || emitted > 5000) {
                return;
            }
            const bool nameOk = name.empty() || globMatch(name, base(abs));
            const bool typeOk =
                type.empty() || (type == "d") == node->directory;
            if (nameOk && typeOk) {
                results.push_back(shown);
                ++emitted;
            }
            if (node->directory && w.fs.canRead(*node, w.user)) {
                std::vector<std::string> names;
                for (const auto &c : node->children) {
                    names.push_back(c.first);
                }
                std::sort(names.begin(), names.end());
                for (const auto &c : names) {
                    walk(VirtualFs::join(abs, c),
                         (shown == "/" ? "" : shown) + "/" + c);
                }
            }
        };
    walk(root, start == "." ? "." : start);
    for (const auto &r : results) {
        io.out += r + "\n";
    }
    return 0;
}

// ---- system and shell commands -------------------------------------------

int cmdUname(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "asnrvm");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    std::vector<std::string> parts;
    const bool all = o.has('a');
    if (all || o.has('s') || o.flags.empty())
        parts.push_back("Linux");
    if (all || o.has('n'))
        parts.push_back(w.hostname);
    if (all || o.has('r'))
        parts.push_back("6.8.0-matterm");
    if (all || o.has('v'))
        parts.push_back("#1 SMP PREEMPT_DYNAMIC");
    if (all || o.has('m'))
        parts.push_back("x86_64");
    if (all)
        parts.push_back("GNU/Linux");
    for (std::size_t i = 0; i < parts.size(); ++i) {
        io.out += (i ? " " : "") + parts[i];
    }
    io.out += "\n";
    return 0;
}

int cmdWhoami(World &w, const Args &, CommandIo &io) {
    io.out += w.user + "\n";
    return 0;
}

int cmdId(World &w, const Args &, CommandIo &io) {
    const bool root = w.user == "root";
    io.out += root ? "uid=0(root) gid=0(root) groups=0(root)\n"
                   : "uid=1000(" + w.user + ") gid=1000(" + w.user +
                         ") groups=1000(" + w.user + ")\n";
    return 0;
}

int cmdHostname(World &w, const Args &, CommandIo &io) {
    io.out += w.hostname + "\n";
    return 0;
}

int cmdDate(World &w, const Args &args, CommandIo &io) {
    const auto t = w.unixNow();
    if (args.size() > 1 && args[1][0] == '+') {
        io.out += unixDate(t, args[1].c_str() + 1) + "\n";
    }
    else {
        io.out += unixDate(t, "%a %b %e %H:%M:%S UTC %Y") + "\n";
    }
    return 0;
}

int cmdEnv(World &w, const Args &, CommandIo &io) {
    for (const auto &[k, v] : w.env) {
        io.out += k + "=" + v + "\n";
    }
    return 0;
}

int cmdExport(World &w, const Args &args, CommandIo &io) {
    if (args.size() == 1) {
        for (const auto &[k, v] : w.env) {
            io.out += "declare -x " + k + "=\"" + v + "\"\n";
        }
        return 0;
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto eq = args[i].find('=');
        if (eq == std::string::npos) {
            continue; // Everything is exported already.
        }
        const auto name = args[i].substr(0, eq);
        bool valid = !name.empty() && !std::isdigit((unsigned char)name[0]);
        for (char c : name) {
            valid = valid && (std::isalnum((unsigned char)c) || c == '_');
        }
        if (!valid) {
            io.err +=
                "matterm: export: `" + args[i] + "': not a valid identifier\n";
            return 1;
        }
        w.env[name] = args[i].substr(eq + 1);
    }
    return 0;
}

int cmdUnset(World &w, const Args &args, CommandIo &) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        w.env.erase(args[i]);
    }
    return 0;
}

int cmdPs(World &w, const Args &, CommandIo &io) {
    io.out += "    PID TTY          TIME CMD\n";
    io.out += "      1 ?        00:00:01 init\n";
    io.out += "     42 pts/0    00:00:00 matterm-sh\n";
    io.out += "    137 pts/0    00:00:00 ps\n";
    (void)w;
    return 0;
}

int cmdUptime(World &w, const Args &, CommandIo &io) {
    const auto up =
        std::chrono::duration_cast<std::chrono::seconds>(w.now).count() +
        3 * 86400 + 4 * 3600 + 17 * 60;
    const auto c = civil(w.unixNow());
    io.out += " " + two(c.hour) + ":" + two(c.minute) + ":" + two(c.second) +
              " up " + std::to_string(up / 86400) + " days, " +
              std::to_string(up % 86400 / 3600) + ":" +
              two(int(up % 3600 / 60)) +
              ",  1 user,  load average: 0.00, 0.01, 0.00\n";
    return 0;
}

int cmdClear(World &, const Args &, CommandIo &io) {
    io.out += "\x1b[H\x1b[2J\x1b[3J";
    return 0;
}

int cmdReset(World &w, const Args &, CommandIo &) {
    w.resetRequest = true;
    return 0;
}

int cmdExit(World &w, const Args &args, CommandIo &io) {
    int code = w.lastStatus;
    if (args.size() > 1) {
        char *end = nullptr;
        const long v = std::strtol(args[1].c_str(), &end, 10);
        if (*end || args[1].empty()) {
            io.err +=
                "matterm: exit: " + args[1] + ": numeric argument required\n";
            w.exitRequest = 2;
            return 2;
        }
        code = int(v);
    }
    w.exitRequest = code & 255;
    return code & 255;
}

int cmdTrue(World &, const Args &, CommandIo &) {
    return 0;
}
int cmdFalse(World &, const Args &, CommandIo &) {
    return 1;
}

int cmdHistory(World &w, const Args &, CommandIo &io) {
    for (std::size_t i = 0; i < w.history.size(); ++i) {
        const auto n = std::to_string(i + 1);
        io.out += std::string(5 - std::min<std::size_t>(4, n.size()), ' ') + n +
                  "  " + w.history[i] + "\n";
    }
    return 0;
}

int cmdColors(World &, const Args &, CommandIo &io) {
    for (int i = 0; i < 8; ++i)
        io.out += "\x1b[4" + std::to_string(i) + "m  ";
    io.out += "\x1b[0m\n";
    for (int i = 0; i < 8; ++i)
        io.out += "\x1b[10" + std::to_string(i) + "m  ";
    io.out += "\x1b[0m\n";
    for (int i = 0; i < 64; ++i) {
        const int v = 255 * i / 63;
        io.out += "\x1b[48;2;" + std::to_string(v) + ";" +
                  std::to_string(255 - v) + ";160m ";
    }
    io.out += "\x1b[0m\n";
    io.out += "\x1b[1mbold\x1b[0m \x1b[2mfaint\x1b[0m \x1b[3mitalic\x1b[0m "
              "\x1b[4munderline\x1b[0m \x1b[7minverse\x1b[0m\n";
    return 0;
}

int cmdWhich(World &w, const Args &args, CommandIo &io) {
    int status = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto &name = args[i];
        if (const auto *c = findCommand(name); c && c->builtin) {
            io.out += name + ": shell built-in command\n";
            continue;
        }
        std::string path = w.env.count("PATH") ? w.env["PATH"] : "";
        bool found = false;
        std::stringstream dirs(path);
        std::string dir;
        while (std::getline(dirs, dir, ':')) {
            const auto *node = w.fs.find(VirtualFs::join(dir, name));
            if (node && !node->directory && (node->mode & 0111)) {
                io.out += VirtualFs::join(dir, name) + "\n";
                found = true;
                break;
            }
        }
        if (!found) {
            status = 1;
        }
    }
    return status;
}

int cmdHelp(World &, const Args &, CommandIo &io) {
    io.out +=
        "matterm pretend system. A small Unix that lives in your browser.\n"
        "Nothing here touches your real computer.\n\n"
        "Commands:\n";
    std::string line;
    for (const auto &c : commands()) {
        const std::string entry = std::string(c.name);
        if (line.size() + entry.size() + 2 > 70) {
            io.out += "  " + line + "\n";
            line.clear();
        }
        line += entry + " ";
    }
    io.out += "  " + line + "\n\n";
    io.out += "Try: ls, cd, cat README, echo hi | wc, grep -n the "
              "/usr/share/dict/words\n"
              "Use Tab to complete, Up for history. There is more than what is "
              "in your PATH.\n";
    return 0;
}

int cmdMan(World &, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "What manual page do you want?\n";
        return 1;
    }
    if (const auto *c = findCommand(args[1])) {
        io.out += std::string(c->name) + " - " + c->summary + "\n";
        return 0;
    }
    io.err += "No manual entry for " + args[1] + "\n";
    return 16;
}

int cmdTree(World &w, const Args &args, CommandIo &io) {
    const auto start = args.size() > 1 ? args[1] : ".";
    const auto root = w.path(start);
    const auto *node = w.fs.find(root);
    if (!node) {
        io.err += start + " [error opening dir]\n";
        return 2;
    }
    io.out += start + "\n";
    int dirs = 0, files = 0;
    std::function<void(const std::string &, const std::string &, int)> walk =
        [&](const std::string &abs, const std::string &prefix, int depth) {
            const auto *n = w.fs.find(abs);
            if (!n || !n->directory || depth > 8 || !w.fs.canRead(*n, w.user)) {
                return;
            }
            std::vector<std::string> names;
            for (const auto &c : n->children) {
                if (c.first[0] != '.')
                    names.push_back(c.first);
            }
            std::sort(names.begin(), names.end(), nameLess);
            for (std::size_t i = 0; i < names.size(); ++i) {
                const bool last = i + 1 == names.size();
                const auto childPath = VirtualFs::join(abs, names[i]);
                const auto *child = w.fs.find(childPath);
                io.out += prefix + (last ? "`-- " : "|-- ") + names[i] + "\n";
                if (child->directory) {
                    ++dirs;
                    walk(childPath,
                         prefix + (last ? "    " : "|   "),
                         depth + 1);
                }
                else {
                    ++files;
                }
            }
        };
    walk(root, "", 0);
    io.out += "\n" + std::to_string(dirs) + " directories, " +
              std::to_string(files) + " files\n";
    return 0;
}

} // namespace

// ---- public helpers ------------------------------------------------------

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto end = text.find('\n', i);
        lines.push_back(
            text.substr(i, end == std::string::npos ? end : end - i));
        if (end == std::string::npos) {
            break;
        }
        i = end + 1;
    }
    return lines;
}

bool globMatch(const std::string &pattern, const std::string &text) {
    std::size_t p = 0, t = 0, starP = std::string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
            continue;
        }
        bool matched = false;
        std::size_t next = p;
        if (p < pattern.size()) {
            if (pattern[p] == '?') {
                matched = true;
                next = p + 1;
            }
            else if (pattern[p] == '[') {
                const auto close = pattern.find(']', p + 2);
                if (close != std::string::npos) {
                    bool negate =
                        pattern[p + 1] == '!' || pattern[p + 1] == '^';
                    bool in = false;
                    for (std::size_t k = p + 1 + negate; k < close; ++k) {
                        if (k + 2 < close && pattern[k + 1] == '-') {
                            in = in || (text[t] >= pattern[k] &&
                                        text[t] <= pattern[k + 2]);
                            k += 2;
                        }
                        else {
                            in = in || text[t] == pattern[k];
                        }
                    }
                    matched = in != negate;
                    next = close + 1;
                }
                else {
                    matched = pattern[p] == text[t];
                    next = p + 1;
                }
            }
            else if (pattern[p] == '\\' && p + 1 < pattern.size()) {
                matched = pattern[p + 1] == text[t];
                next = p + 2;
            }
            else {
                matched = pattern[p] == text[t];
                next = p + 1;
            }
        }
        if (matched) {
            p = next;
            ++t;
        }
        else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        }
        else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::string unixDate(std::int64_t seconds, const char *format) {
    const auto c = civil(seconds);
    std::string out;
    for (const char *f = format; *f; ++f) {
        if (*f != '%' || !f[1]) {
            out += *f;
            continue;
        }
        switch (*++f) {
        case 'Y':
            out += std::to_string(c.year);
            break;
        case 'm':
            out += two(c.month);
            break;
        case 'd':
            out += two(c.day);
            break;
        case 'e':
            out += (c.day < 10 ? " " : "") + std::to_string(c.day);
            break;
        case 'H':
            out += two(c.hour);
            break;
        case 'M':
            out += two(c.minute);
            break;
        case 'S':
            out += two(c.second);
            break;
        case 'a':
            out += kDays[c.weekday];
            break;
        case 'b':
            out += kMonths[c.month - 1];
            break;
        case 's':
            out += std::to_string(seconds);
            break;
        case 'F':
            out +=
                std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day);
            break;
        case 'T':
            out += two(c.hour) + ":" + two(c.minute) + ":" + two(c.second);
            break;
        case '%':
            out += '%';
            break;
        default:
            out += '%';
            out += *f;
            break;
        }
    }
    return out;
}

const std::vector<CommandInfo> &commands() {
    static const std::vector<CommandInfo> table = {
        {"cd", cmdCd, true, "change the working directory"},
        {"echo", cmdEcho, true, "print arguments"},
        {"pwd", cmdPwd, true, "print the working directory"},
        {"export", cmdExport, true, "set environment variables"},
        {"unset", cmdUnset, true, "remove environment variables"},
        {"history", cmdHistory, true, "show the command history"},
        {"clear", cmdClear, true, "clear the screen"},
        {"reset",
         cmdReset,
         true,
         "restore the pretend system to its first state"},
        {"exit", cmdExit, true, "end the session"},
        {"help", cmdHelp, true, "list commands"},
        {"true", cmdTrue, true, "succeed"},
        {"false", cmdFalse, true, "fail"},
        {"ls", cmdLs, false, "list directory contents"},
        {"cat", cmdCat, false, "print files"},
        {"mkdir", cmdMkdir, false, "make directories"},
        {"touch", cmdTouch, false, "create files or update their time"},
        {"rm", cmdRm, false, "remove files"},
        {"cp", cmdCp, false, "copy files"},
        {"mv", cmdMv, false, "move or rename files"},
        {"head", cmdHead, false, "print the first lines"},
        {"tail", cmdTail, false, "print the last lines"},
        {"wc", cmdWc, false, "count lines, words and bytes"},
        {"grep", cmdGrep, false, "search for matching lines"},
        {"sort", cmdSort, false, "sort lines"},
        {"uniq", cmdUniq, false, "collapse repeated lines"},
        {"seq", cmdSeq, false, "print a sequence of numbers"},
        {"yes", cmdYes, false, "repeat a line"},
        {"find", cmdFind, false, "search for files"},
        {"tree", cmdTree, false, "show a directory tree"},
        {"uname", cmdUname, false, "print system information"},
        {"whoami", cmdWhoami, false, "print the user name"},
        {"id", cmdId, false, "print user and group ids"},
        {"hostname", cmdHostname, false, "print the host name"},
        {"date", cmdDate, false, "print the date and time"},
        {"env", cmdEnv, false, "print the environment"},
        {"ps", cmdPs, false, "list processes"},
        {"uptime", cmdUptime, false, "show how long the system has run"},
        {"colors", cmdColors, false, "show the terminal's colors"},
        {"which", cmdWhich, false, "locate a command"},
        {"man", cmdMan, false, "show a manual entry"},
    };
    return table;
}

const CommandInfo *findCommand(const std::string &name) {
    for (const auto &c : commands()) {
        if (name == c.name) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace fakeos
