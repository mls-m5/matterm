#include "commands.h"

#include "hardware.h"
#include "programs.h"

#if defined(__has_include)
#if __has_include("fakeos_about_stats.h")
#include "fakeos_about_stats.h"
#define FAKEOS_HAVE_ABOUT_STATS 1
#endif
#endif

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
const char *kFullMonths[] = {"January",
                             "February",
                             "March",
                             "April",
                             "May",
                             "June",
                             "July",
                             "August",
                             "September",
                             "October",
                             "November",
                             "December"};
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
    const auto o = parse(args, "asnrvmo");
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
        parts.push_back("6.8.0-mattrix");
    if (all || o.has('v'))
        parts.push_back("#6 SMP Professional Edition");
    if (all || o.has('m'))
        parts.push_back("x86_64");
    if (all || o.has('o'))
        parts.push_back("Mattrix OS 6.0 Professional Edition");
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
    io.out += "     42 pts/0    00:00:00 mattrix-sh\n";
    io.out += "    137 pts/0    00:00:00 ps\n";
    (void)w;
    return 0;
}

// The calendar span from the boot to now: "38 years, 10 months, 5 days".
std::string spanSinceBoot(const World &w) {
    const auto from = civil(hardware::kBootUnix), to = civil(w.unixNow());
    int years = to.year - from.year, months = to.month - from.month,
        days = to.day - from.day;
    if (days < 0) {
        --months;
        // Days in the month before `to`.
        static const int lengths[] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const int previous = to.month == 1 ? 12 : to.month - 1;
        const int year = to.month == 1 ? to.year - 1 : to.year;
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        days += lengths[previous - 1] + (previous == 2 && leap);
    }
    if (months < 0) {
        --years;
        months += 12;
    }
    const auto seconds = w.uptimeSeconds();
    return std::to_string(years) + " years, " + std::to_string(months) +
           " months, " + std::to_string(days) + " days, " +
           std::to_string(seconds % 86400 / 3600) + " hours, " +
           std::to_string(seconds % 3600 / 60) + " mins";
}

int cmdUptime(World &w, const Args &args, CommandIo &io) {
    const auto up = w.uptimeSeconds();
    const auto o = parse(args, "ps");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    if (o.has('s')) {
        io.out += unixDate(hardware::kBootUnix, "%F %T") + "\n";
        return 0;
    }
    if (o.has('p')) {
        io.out += "up " + std::to_string(up / 604800) + " weeks, " +
                  std::to_string(up % 604800 / 86400) + " days, " +
                  std::to_string(up % 86400 / 3600) + " hours, " +
                  std::to_string(up % 3600 / 60) + " minutes\n";
        return 0;
    }
    const auto c = civil(w.unixNow());
    const auto hours = std::to_string(up % 86400 / 3600);
    io.out += " " + two(c.hour) + ":" + two(c.minute) + ":" + two(c.second) +
              " up " + std::to_string(up / 86400) + " days, " +
              std::string(hours.size() < 2 ? " " : "") + hours + ":" +
              two(int(up % 3600 / 60)) +
              ",  1 user,  load average: 0.00, 0.01, 0.00\n";
    return 0;
}

// who: who is logged in, and when the system booted.
int cmdWho(World &w, const Args &args, CommandIo &io) {
    const auto o = parse(args, "b");
    if (!o.bad.empty()) {
        return badOption(args, o, io);
    }
    if (o.has('b')) {
        io.out += "         system boot  " +
                  unixDate(hardware::kBootUnix, "%F %H:%M") + "\n";
        return 0;
    }
    io.out += w.user + "    pts/0        " + unixDate(w.bootUnix, "%F %H:%M") +
              " (:0)\n";
    return 0;
}

// last [reboot]: the logins and the one reboot, long ago.
int cmdLast(World &w, const Args &args, CommandIo &io) {
    const bool onlyReboot = args.size() > 1 && args[1] == "reboot";
    if (args.size() > 1 && !onlyReboot) {
        io.err += "last: no such user: " + args[1] + "\n";
        return 1;
    }
    if (!onlyReboot) {
        io.out += w.user + "    pts/0        :0               " +
                  unixDate(w.bootUnix, "%a %b %e %H:%M") +
                  "   still logged in\n";
    }
    io.out += "reboot   system boot  6.8.0-mattrix " +
              unixDate(hardware::kBootUnix, "%a %b %e %H:%M") +
              "   still running\n\nwtmp begins " +
              unixDate(hardware::kBootUnix, "%a %b %e %H:%M:%S %Y") + "\n";
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

int cmdSound(World &w, const Args &args, CommandIo &io) {
    if (args.size() > 2 ||
        (args.size() == 2 && args[1] != "on" && args[1] != "off")) {
        io.err += "usage: sound on|off\n";
        return 2;
    }
    if (!w.soundAvailable) {
        io.err += "Sound is not available here.\n";
        return 1;
    }
    if (args.size() == 2) {
        w.sound = args[1] == "on";
        w.fs.put("/etc/motd", motdText(w), "root", 0644, w.unixNow());
    }
    io.out +=
        std::string("Sound: ") + (w.sound ? "on" : "off") + " (sound on|off)\n";
    return 0;
}

int cmdSlow(World &w, const Args &args, CommandIo &io) {
    if (args.size() > 2 ||
        (args.size() == 2 && args[1] != "on" && args[1] != "off")) {
        io.err += "usage: slow on|off\n";
        return 2;
    }
    if (args.size() == 2) {
        w.slow = args[1] == "on";
        w.slowAsked = w.slowAsked || w.slow;
        w.fs.put("/etc/motd", motdText(w), "root", 0644, w.unixNow());
    }
    io.out += std::string("Dramatic typing speed: ") +
              (w.slow ? "enabled" : "disabled") + " (slow on|off)\n";
    return 0;
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
    io.out += "Mattrix OS 6.0 Professional Edition, a simulated system.\n"
              "A small Unix that lives in a terminal.\n"
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
    io.out +=
        "Try: fire, gradient, neofetch, about, cal, cowsay hello, cat README,\n"
        "     echo hi | wc, grep -n the /usr/share/dict/words\n"
        "Dramatic typing speed: 'slow off' turns it off, 'slow on' back on "
        "(Space skips).\n"
        "Sound: 'sound on' turns the typing pips on, 'sound off' off.\n"
        "Use Tab to complete, Up for history. There is more to find than "
        "what is in your PATH.\n";
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

// ---- more commands -------------------------------------------------------

std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = unsigned(y - era * 400);
    const auto doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + std::int64_t(doe) - 719468;
}

int cmdCal(World &w, const Args &args, CommandIo &io) {
    const auto today = civil(w.unixNow());
    int month = today.month, year = today.year;
    if (args.size() == 3) {
        month = toInt(args[1], 0);
        year = toInt(args[2], 0);
    }
    else if (args.size() == 2) {
        year = toInt(args[1], 0);
        month = 0;
    }
    if (year < 1 || year > 9999 || month < 0 || month > 12) {
        io.err += "cal: invalid date\n";
        return 1;
    }
    auto one = [&](int m) {
        std::string out;
        const std::string title =
            std::string(kFullMonths[m - 1]) + " " + std::to_string(year);
        out += std::string((20 - std::min<std::size_t>(20, title.size())) / 2,
                           ' ') +
               title + "\n";
        out += "Su Mo Tu We Th Fr Sa\n";
        // 1970-01-01 was a Thursday (4 when Sunday is 0).
        const auto first = ((daysFromCivil(year, m, 1) % 7 + 7) % 7 + 4) % 7;
        static const int lengths[] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        const int days = lengths[m - 1] + (m == 2 && leap);
        std::string line(std::size_t(first) * 3, ' ');
        for (int d = 1; d <= days; ++d) {
            std::string cell = (d < 10 ? " " : "") + std::to_string(d);
            if (io.tty && d == today.day && m == today.month &&
                year == today.year) {
                cell = "\x1b[7m" + cell + "\x1b[0m";
            }
            line += cell;
            if ((first + d) % 7 == 0 || d == days) {
                out += line + "\n";
                line.clear();
            }
            else {
                line += " ";
            }
        }
        return out;
    };
    if (month == 0) {
        for (int m = 1; m <= 12; ++m) {
            io.out += one(m) + "\n";
        }
    }
    else {
        io.out += one(month);
    }
    return 0;
}

int cmdFortune(World &w, const Args &, CommandIo &io) {
    static const char *quotes[] = {
        "A journey of a thousand miles begins with a single `ls`.",
        "There is no place like 127.0.0.1.",
        "Programs must be written for people to read, and only incidentally "
        "for machines to execute.",
        "The best way to predict the future is to invent it.",
        "Everything should be made as simple as possible, but no simpler.",
        "Premature optimization is the root of all evil.",
        "To iterate is human, to recurse divine.",
        "It works on my machine. (This machine is imaginary.)",
        "Not all those who wander the file system are lost.",
        "Have you tried turning it off and on again? Try `reset`.",
        "In the beginning was the command line.",
        "Look in the dark corners. Look in /usr/games.",
    };
    io.out +=
        std::string(quotes[w.rng.below(sizeof quotes / sizeof quotes[0])]) +
        "\n";
    return 0;
}

int cmdCowsay(World &, const Args &args, CommandIo &io) {
    std::string text;
    for (std::size_t i = 1; i < args.size(); ++i) {
        text += (i > 1 ? " " : "") + args[i];
    }
    if (text.empty()) {
        text = io.in;
        while (!text.empty() && text.back() == '\n')
            text.pop_back();
    }
    if (text.empty()) {
        text = "Moo.";
    }
    // Wrap at 40 columns.
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (!line.empty() && line.size() + 1 + word.size() > 40) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
    }
    lines.push_back(line);
    std::size_t width = 0;
    for (const auto &l : lines)
        width = std::max(width, l.size());
    io.out += " " + std::string(width + 2, '_') + "\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const char open = lines.size() == 1       ? '<'
                          : i == 0                ? '/'
                          : i + 1 == lines.size() ? '\\'
                                                  : '|';
        const char close = lines.size() == 1       ? '>'
                           : i == 0                ? '\\'
                           : i + 1 == lines.size() ? '/'
                                                   : '|';
        io.out += std::string(1, open) + " " + lines[i] +
                  std::string(width - lines[i].size(), ' ') + " " + close +
                  "\n";
    }
    io.out += " " + std::string(width + 2, '-') +
              "\n"
              "        \\   ^__^\n"
              "         \\  (oo)\\_______\n"
              "            (__)\\       )\\/\\\n"
              "                ||----w |\n"
              "                ||     ||\n";
    return 0;
}

int cmdRev(World &w, const Args &args, CommandIo &io) {
    std::string text;
    const bool ok =
        inputOf(w, "rev", Args(args.begin() + 1, args.end()), text, io);
    for (const auto &l : splitLines(text)) {
        // Reverse by code points so UTF-8 survives.
        std::vector<std::string> chars;
        for (std::size_t i = 0; i < l.size();) {
            const auto b = (unsigned char)l[i];
            const std::size_t n = b < 0x80    ? 1
                                  : b >= 0xF0 ? 4
                                  : b >= 0xE0 ? 3
                                  : b >= 0xC0 ? 2
                                              : 1;
            chars.push_back(l.substr(i, n));
            i += n;
        }
        for (auto it = chars.rbegin(); it != chars.rend(); ++it)
            io.out += *it;
        io.out += "\n";
    }
    return ok ? 0 : 1;
}

int cmdTac(World &w, const Args &args, CommandIo &io) {
    std::string text;
    const bool ok =
        inputOf(w, "tac", Args(args.begin() + 1, args.end()), text, io);
    auto lines = splitLines(text);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        io.out += *it + "\n";
    return ok ? 0 : 1;
}

int cmdNl(World &w, const Args &args, CommandIo &io) {
    std::string text;
    const bool ok =
        inputOf(w, "nl", Args(args.begin() + 1, args.end()), text, io);
    int n = 1;
    for (const auto &l : splitLines(text)) {
        if (l.empty()) {
            io.out += "\n";
            continue;
        }
        const auto num = std::to_string(n++);
        io.out += std::string(6 - std::min<std::size_t>(5, num.size()), ' ') +
                  num + "\t" + l + "\n";
    }
    return ok ? 0 : 1;
}

std::string expandSet(const std::string &set) {
    std::string out;
    for (std::size_t i = 0; i < set.size(); ++i) {
        char c = set[i];
        if (c == '\\' && i + 1 < set.size()) {
            const char e = set[++i];
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        if (i + 2 < set.size() && set[i + 1] == '-' && set[i + 2] >= c) {
            for (int ch = (unsigned char)c; ch <= (unsigned char)set[i + 2];
                 ++ch)
                out += char(ch);
            i += 2;
        }
        else {
            out += c;
        }
    }
    return out;
}

int cmdTr(World &, const Args &args, CommandIo &io) {
    bool del = false, squeeze = false;
    Args sets;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-d")
            del = true;
        else if (args[i] == "-s")
            squeeze = true;
        else
            sets.push_back(args[i]);
    }
    if (sets.empty() || (!del && !squeeze && sets.size() < 2)) {
        io.err += "tr: missing operand\n";
        return 1;
    }
    const auto from = expandSet(sets[0]);
    auto to = sets.size() > 1 ? expandSet(sets[1]) : std::string();
    if (!to.empty() && to.size() < from.size())
        to.append(from.size() - to.size(), to.back());
    std::string out;
    char last = 0;
    for (const char c : io.in) {
        const auto at = from.find(c);
        if (del) {
            if (at == std::string::npos)
                out += c;
            continue;
        }
        const char mapped = at != std::string::npos && !to.empty() ? to[at] : c;
        if (squeeze && at != std::string::npos && mapped == last)
            continue;
        out += mapped;
        last = mapped;
    }
    io.out += out;
    return 0;
}

int cmdCut(World &w, const Args &args, CommandIo &io) {
    char delimiter = '\t';
    std::string fields, chars;
    Args files;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-d" && i + 1 < args.size())
            delimiter = args[++i].empty() ? '\t' : args[i][0];
        else if (args[i].compare(0, 2, "-d") == 0 && args[i].size() > 2)
            delimiter = args[i][2];
        else if (args[i] == "-f" && i + 1 < args.size())
            fields = args[++i];
        else if (args[i].compare(0, 2, "-f") == 0)
            fields = args[i].substr(2);
        else if (args[i] == "-c" && i + 1 < args.size())
            chars = args[++i];
        else if (args[i].compare(0, 2, "-c") == 0)
            chars = args[i].substr(2);
        else
            files.push_back(args[i]);
    }
    if (fields.empty() && chars.empty()) {
        io.err +=
            "cut: you must specify a list of bytes, characters, or fields\n";
        return 1;
    }
    // A list like "1,3-4": expand to 1-based numbers.
    auto parseList = [&](const std::string &list) {
        std::vector<int> picks;
        std::stringstream parts(list);
        std::string part;
        while (std::getline(parts, part, ',')) {
            const auto dash = part.find('-');
            if (dash == std::string::npos)
                picks.push_back(toInt(part, 0));
            else {
                const int a = toInt(part.substr(0, dash), 1);
                const int b = toInt(part.substr(dash + 1), 200);
                for (int n = a; n <= std::min(b, 200); ++n)
                    picks.push_back(n);
            }
        }
        return picks;
    };
    std::string text;
    const bool ok = inputOf(w, "cut", files, text, io);
    const auto picks = parseList(fields.empty() ? chars : fields);
    for (const auto &l : splitLines(text)) {
        std::string out;
        if (!fields.empty()) {
            std::vector<std::string> cols;
            std::size_t begin = 0;
            for (;;) {
                const auto end = l.find(delimiter, begin);
                cols.push_back(l.substr(
                    begin, end == std::string::npos ? end : end - begin));
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            if (cols.size() == 1 && l.find(delimiter) == std::string::npos) {
                io.out += l + "\n";
                continue;
            }
            bool first = true;
            for (const int n : picks) {
                if (n >= 1 && std::size_t(n) <= cols.size()) {
                    out += (first ? "" : std::string(1, delimiter)) +
                           cols[std::size_t(n) - 1];
                    first = false;
                }
            }
        }
        else {
            for (const int n : picks) {
                if (n >= 1 && std::size_t(n) <= l.size())
                    out += l[std::size_t(n) - 1];
            }
        }
        io.out += out + "\n";
    }
    return ok ? 0 : 1;
}

int cmdBasename(World &, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "basename: missing operand\n";
        return 1;
    }
    auto path = args[1];
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    auto name = path == "/" ? std::string("/")
                            : path.substr(path.rfind('/') == std::string::npos
                                              ? 0
                                              : path.rfind('/') + 1);
    if (args.size() > 2 && name.size() > args[2].size() &&
        name.compare(name.size() - args[2].size(), args[2].size(), args[2]) ==
            0) {
        name.resize(name.size() - args[2].size());
    }
    io.out += name + "\n";
    return 0;
}

int cmdDirname(World &, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "dirname: missing operand\n";
        return 1;
    }
    auto path = args[1];
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    const auto slash = path.rfind('/');
    io.out += (slash == std::string::npos ? "."
               : slash == 0               ? "/"
                                          : path.substr(0, slash)) +
              "\n";
    return 0;
}

int cmdSleep(World &, const Args &, CommandIo &) {
    return 0; // Nothing waits in a simulated system.
}

std::string pad(const std::string &text, std::size_t width) {
    return std::string(width > text.size() ? width - text.size() : 0, ' ') +
           text;
}

// Used space on the root disk: what the system itself takes, and the visitor's
// own files.
long long diskUsedKiB(const World &w) {
    return hardware::kDiskBaseUsedKiB + (long long)(w.fs.totalBytes() / 1024);
}

int cmdDf(World &w, const Args &, CommandIo &io) {
    const long long total = hardware::kDiskTotalKiB, used = diskUsedKiB(w);
    const long long tmp = hardware::kTmpTotalKiB;
    io.out += "Filesystem         1K-blocks        Used   Available Use% "
              "Mounted on\n";
    io.out += "/dev/nvme0n1p2 " + pad(std::to_string(total), 13) + " " +
              pad(std::to_string(used), 11) + " " +
              pad(std::to_string(total - used), 11) + " " +
              pad(std::to_string(used * 100 / total), 3) + "% /\n";
    io.out += "tmpfs          " + pad(std::to_string(tmp), 13) + " " +
              pad("0", 11) + " " + pad(std::to_string(tmp), 11) +
              "   0% /tmp\n";
    return 0;
}

int cmdFree(World &, const Args &, CommandIo &io) {
    using namespace hardware;
    io.out += "               total        used        free      shared  "
              "buff/cache   available\n";
    io.out += "Mem:    " + pad(std::to_string(kRamTotalKiB), 12) +
              pad(std::to_string(kRamUsedKiB), 12) +
              pad(std::to_string(kRamFreeKiB), 12) +
              pad(std::to_string(kRamSharedKiB), 12) +
              pad(std::to_string(kRamCacheKiB), 12) +
              pad(std::to_string(kRamAvailableKiB), 12) + "\n";
    io.out += "Swap:   " + pad("0", 12) + pad("0", 12) + pad("0", 12) + "\n";
    return 0;
}

int cmdNproc(World &, const Args &, CommandIo &io) {
    io.out += std::to_string(hardware::kCpuCores) + "\n";
    return 0;
}

int cmdPrintf(World &, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "printf: usage: printf format [arguments]\n";
        return 2;
    }
    const auto &f = args[1];
    std::size_t next = 2;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '\\' && i + 1 < f.size()) {
            const char e = f[++i];
            io.out += e == 'n'    ? '\n'
                      : e == 't'  ? '\t'
                      : e == 'e'  ? '\x1b'
                      : e == '\\' ? '\\'
                                  : e;
        }
        else if (f[i] == '%' && i + 1 < f.size()) {
            const char k = f[++i];
            if (k == '%')
                io.out += '%';
            else if (k == 's')
                io.out += next < args.size() ? args[next++] : "";
            else if (k == 'd')
                io.out += std::to_string(
                    next < args.size() ? toInt(args[next++], 0) : 0);
            else {
                io.out += '%';
                io.out += k;
            }
        }
        else {
            io.out += f[i];
        }
    }
    return 0;
}

int cmdSed(World &w, const Args &args, CommandIo &io) {
    if (args.size() < 2) {
        io.err += "sed: no script given\n";
        return 1;
    }
    const auto &script = args[1];
    if (script.size() < 4 || script[0] != 's') {
        io.err += "sed: only s/pattern/replacement/[g] is supported\n";
        return 1;
    }
    const char d = script[1];
    const auto a = script.find(d, 2);
    const auto b = a == std::string::npos ? a : script.find(d, a + 1);
    if (b == std::string::npos) {
        io.err += "sed: unterminated `s' command\n";
        return 1;
    }
    const auto pattern = script.substr(2, a - 2);
    const auto replacement = script.substr(a + 1, b - a - 1);
    const auto flags = script.substr(b + 1);
    std::regex re;
    try {
        re = std::regex(pattern,
                        flags.find('i') != std::string::npos
                            ? std::regex::icase | std::regex::ECMAScript
                            : std::regex::ECMAScript);
    }
    catch (const std::regex_error &) {
        io.err += "sed: invalid regular expression\n";
        return 1;
    }
    std::string text;
    const bool ok =
        inputOf(w, "sed", Args(args.begin() + 2, args.end()), text, io);
    for (const auto &l : splitLines(text)) {
        io.out +=
            std::regex_replace(l,
                               re,
                               replacement,
                               flags.find('g') != std::string::npos
                                   ? std::regex_constants::format_default
                                   : std::regex_constants::format_first_only) +
            "\n";
    }
    return ok ? 0 : 1;
}

// An old CRT monitor on its computer, a keyboard in front, drawn with markers:
// \x01 body, \x02 screen, \x03 the name on the screen, \x04 knobs, \x05 back to
// plain.
const char *const kTv[] = {
    ""
    "\x05"
    "  "
    "\x01"
    ".------------------."
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    ".--------------."
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x02"
    "              "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x02"
    "   "
    "\x03"
    "MATTRIX"
    "\x02"
    "    "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x02"
    "     "
    "\x03"
    "O S"
    "\x02"
    "      "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x02"
    "              "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "'--------------'"
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "'------------------'"
    "\x05"
    "  "
    "\x05"
    "",
    ""
    "\x05"
    "      "
    "\x01"
    "/__________\\"
    "\x05"
    "      "
    "\x05"
    "",
    ""
    "\x05"
    " "
    "\x01"
    ".--------------------."
    "\x05"
    " "
    "\x05"
    "",
    ""
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    " "
    "\x01"
    "[======]"
    "\x05"
    "     "
    "\x04"
    "o  o"
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x05"
    "",
    ""
    "\x05"
    " "
    "\x01"
    "|"
    "\x05"
    "  "
    "\x01"
    "::::::::::::::::"
    "\x05"
    "  "
    "\x01"
    "|"
    "\x05"
    " "
    "\x05"
    "",
    ""
    "\x05"
    " "
    "\x01"
    "'--------------------'"
    "\x05"
    " "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    ".-----------------."
    "\x05"
    "   "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "|_|_|_|_|_|_|_|_|_|"
    "\x05"
    "   "
    "\x05"
    "",
    ""
    "\x05"
    "  "
    "\x01"
    "'-----------------'"
    "\x05"
    "   "
    "\x05"
    "",
    "\x02"
    "     by Mattias Lasersk\xc3\xb6"
    "ld"
    "\x05",
};

int cmdNeofetch(World &w, const Args &, CommandIo &io) {
    const std::string account = w.user + "@" + w.hostname;
    struct Line {
        std::string label, value;
    };
    const std::vector<Line> info = {
        {"OS", "Mattrix OS 6.0 Professional Edition x86_64"},
        {"Host", w.hostname + " (a simulated machine)"},
        {"Kernel", "6.8.0-mattrix"},
        {"Uptime", spanSinceBoot(w)},
        {"Packages", "42 (mattrix)"},
        {"Shell", "mattrix-sh 1.0"},
        {"Display", hardware::kDisplay},
        {"Terminal",
         std::to_string(w.columns) + "x" + std::to_string(w.rows) + " cells"},
        {"CPU",
         std::string(hardware::kCpuModel) + " (" +
             std::to_string(hardware::kCpuCores) + ") @ " +
             hardware::kCpuClock},
        {"GPU", hardware::kGpu},
        {"Memory",
         hardware::size(hardware::kRamUsedKiB) + " / " +
             hardware::size(hardware::kRamTotalKiB)},
        {"Disk (/)",
         hardware::size(diskUsedKiB(w)) + " / " +
             hardware::size(hardware::kDiskTotalKiB) + " (" +
             std::to_string(diskUsedKiB(w) * 100 / hardware::kDiskTotalKiB) +
             "%)"},
    };
    std::vector<std::string> right;
    right.push_back(io.tty ? "\x1b[1;32m" + w.user + "\x1b[0m@\x1b[1;32m" +
                                 w.hostname + "\x1b[0m"
                           : account);
    right.push_back(std::string(account.size(), '-'));
    for (const auto &l : info) {
        right.push_back(io.tty ? "\x1b[1;36m" + l.label + "\x1b[0m: " + l.value
                               : l.label + ": " + l.value);
    }
    right.push_back("");
    std::string blocks, bright;
    for (int i = 0; i < 8; ++i) {
        blocks += "\x1b[4" + std::to_string(i) + "m   ";
        bright += "\x1b[10" + std::to_string(i) + "m   ";
    }
    right.push_back(io.tty ? blocks + "\x1b[0m" : "");
    right.push_back(io.tty ? bright + "\x1b[0m" : "");
    const std::size_t rows = std::max(right.size(), sizeof kTv / sizeof kTv[0]);
    for (std::size_t r = 0; r < rows; ++r) {
        std::string left;
        std::size_t width = 0;
        if (r < sizeof kTv / sizeof kTv[0]) {
            for (const char c : std::string(kTv[r])) {
                if ((unsigned char)c <= 5) {
                    if (io.tty) {
                        static const char *colors[] = {"",
                                                       "\x1b[33m",
                                                       "\x1b[36m",
                                                       "\x1b[1;92m",
                                                       "\x1b[31m",
                                                       "\x1b[0m"};
                        left += colors[int(c)];
                    }
                }
                else {
                    left += c;
                    width += ((unsigned char)c & 0xC0) != 0x80; // Code points.
                }
            }
        }
        left += std::string(width < 27 ? 27 - width : 0, ' ');
        io.out += left + (r < right.size() ? right[r] : "") + "\n";
    }
    return 0;
}

int cmdAbout(World &, const Args &, CommandIo &io) {
    const bool c = io.tty;
    auto bold = [&](const std::string &text) {
        return c ? "\x1b[1;36m" + text + "\x1b[0m" : text;
    };
    auto strong = [&](const std::string &text) {
        return c ? "\x1b[1m" + text + "\x1b[0m" : text;
    };
    io.out +=
        strong("about: Mattrix OS 6.0 Professional Edition") +
        "\n\n"
        "matterm is a terminal emulator written from scratch, and this is its\n"
        "demo: a real terminal with a fake operating system.\n\n";
    io.out +=
        bold("Technical background") +
        "\n"
        "  * C++17. The core is a hand-written VT parser and terminal model "
        "that do\n"
        "    no I/O at all: no files, no sockets, no clocks. The same code "
        "runs in\n"
        "    every place below.\n"
        "  * Native on Linux: an SDL2 window and a real PTY with your own "
        "shell.\n"
        "  * In the browser: the same code as WebAssembly (Emscripten) on a "
        "canvas,\n"
        "    with touch: drag to scroll, pinch to zoom, long press to select.\n"
        "  * Its own cell renderer with a glyph atlas. An idle page costs next "
        "to\n"
        "    nothing: the loop throttles itself when nothing changes.\n"
        "  * Its own wire protocol, matterm-tty, and a server for it: "
        "Boost.Beast,\n"
        "    Argon2id password hashing with libsodium, strict Origin checks "
        "and a\n"
        "    script-only Content-Security-Policy.\n"
        "  * Fuzz tests for the VT parser, the protocol and the server, "
        "benchmarks,\n"
        "    and a written security review.\n"
        "  * This demo is a simulated Unix world in plain C++: a virtual file\n"
        "    system, a shell and programs. It runs statically in the browser,\n"
        "    without a server.\n\n";
#ifdef FAKEOS_HAVE_ABOUT_STATS
    const auto &n = kAboutStats;
    io.out +=
        bold("By the numbers") +
        " (counted from the repository when this was built)\n"
        "  " +
        std::to_string(n.cppFiles) + " files, " + std::to_string(n.cppLines) +
        " lines of C++ (libraries, apps, benchmarks, this demo)\n"
        "  " +
        std::to_string(n.webFiles) + " files, " + std::to_string(n.webLines) +
        " lines of JavaScript, HTML and CSS for the browser client\n"
        "  " +
        std::to_string(n.testCases) + " test cases in " +
        std::to_string(n.testFiles) + " test files, " +
        std::to_string(n.testLines) +
        " lines of tests\n"
        "  " +
        std::to_string(n.docFiles) + " documents, " +
        std::to_string(n.docLines) +
        " lines of documents\n"
        "  " +
        std::to_string(n.demoLines) +
        " lines of this demo (src/fakeos and app/demo, " +
        std::to_string(n.demoFiles) + " files)\n\n";
#else
    io.out += bold("By the numbers") + "\n  (not available in this build)\n\n";
#endif
    io.out += strong("Byggt av Mattias Lasersk\xc3\xb6"
                     "ld") +
              "\n"
              "med Claude, Pi coding agent och OpenAI Codex.\n";
    return 0;
}

// Programs started from a command: `less FILE`, `top`, ... The program gets the
// text through the World.
int cmdLess(World &w, const Args &args, CommandIo &io) {
    std::string text;
    if (args.size() > 1) {
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string part;
            if (!readFile(w, "less", args[i], part, io))
                return 1;
            text += part;
        }
    }
    else if (!io.in.empty()) {
        text = io.in;
    }
    else {
        io.err += "Missing filename (\"less --help\" for help)\n";
        return 1;
    }
    w.programInput = text;
    w.launch = World::Launch{"less", {}, {}};
    return 0;
}

int cmdTop(World &w, const Args &, CommandIo &) {
    w.launch = World::Launch{"top", {}, {}};
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
        {"slow", cmdSlow, true, "turn the dramatic typing speed on or off"},
        {"sound", cmdSound, true, "turn the typing pips on or off"},
        {"history", cmdHistory, true, "show the command history"},
        {"clear", cmdClear, true, "clear the screen"},
        {"reset",
         cmdReset,
         true,
         "restore the simulated system to its first state"},
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
        {"who", cmdWho, false, "show who is logged in, and the boot time"},
        {"last", cmdLast, false, "show logins and reboots"},
        {"colors", cmdColors, false, "show the terminal's colors"},
        {"which", cmdWhich, false, "locate a command"},
        {"man", cmdMan, false, "show a manual entry"},
        {"cal", cmdCal, false, "show a calendar"},
        {"fortune", cmdFortune, false, "print a saying"},
        {"cowsay", cmdCowsay, false, "a cow says it"},
        {"rev", cmdRev, false, "reverse each line"},
        {"tac", cmdTac, false, "print lines in reverse order"},
        {"nl", cmdNl, false, "number lines"},
        {"tr", cmdTr, false, "translate or delete characters"},
        {"cut", cmdCut, false, "select fields or characters"},
        {"sed", cmdSed, false, "replace text with s/old/new/"},
        {"printf", cmdPrintf, false, "format and print"},
        {"basename", cmdBasename, false, "strip the directory from a path"},
        {"dirname", cmdDirname, false, "strip the last part of a path"},
        {"sleep", cmdSleep, false, "wait (instantly, here)"},
        {"df", cmdDf, false, "show disk space"},
        {"free", cmdFree, false, "show memory"},
        {"nproc", cmdNproc, false, "print the number of processors"},
        {"less", cmdLess, false, "page through a file"},
        {"neofetch", cmdNeofetch, false, "show the system with a picture"},
        {"about", cmdAbout, false, "what this is made of, and by whom"},
        {"sysinfo", cmdNeofetch, false, "show the system with a picture"},
        {"top", cmdTop, false, "watch the processes"},
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
