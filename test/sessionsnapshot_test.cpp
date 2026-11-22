#include "mls-unit-test/unittest.h"
#include "paneprobe.h"
#include "sessionsnapshot.h"

#include <map>
#include <random>

using namespace frontend;

namespace {
SavedPane pane(std::string cwd = "/home/a", std::vector<std::string> cmd = {}, std::string text = {}) {
    SavedPane p;
    p.cwd = std::move(cwd);
    p.command = std::move(cmd);
    p.text = std::move(text);
    return p;
}
SessionSnapshot sample() {
    SessionSnapshot s;
    s.name = "work";
    s.saved = 1763800000;
    s.active = 1;
    SavedTab a;
    a.title = "build";
    a.root = SavedNode::split(SplitDirection::SideBySide, 300, SavedNode::leafOf(pane("/src", {"make", "-j", "8"})),
                              SavedNode::split(SplitDirection::Stacked, 700, SavedNode::leafOf(pane("/tmp")),
                                               SavedNode::leafOf(pane("/", {}, "line one\nline two"))));
    a.focus = 2;
    s.tabs.push_back(std::move(a));
    SavedTab b;
    b.root = SavedNode::leafOf(pane("/home/a/ö dir", {"vim", "it's a file.txt"}));
    s.tabs.push_back(std::move(b));
    return s;
}
std::string dumpNode(const SavedNode &n) {
    if (n.leaf) {
        std::string c;
        for (const auto &a : n.pane.command) c += a + "|";
        return "L(" + n.pane.cwd + ";" + c + ";" + n.pane.text + ";" + n.pane.terminal + ")";
    }
    return std::string(n.direction == SplitDirection::SideBySide ? "H" : "V") + std::to_string(n.permille) + "(" +
           dumpNode(*n.first) + "," + dumpNode(*n.second) + ")";
}
std::string dump(const SessionSnapshot &s) {
    std::string o = s.name + "@" + std::to_string(s.saved) + "#" + std::to_string(s.active);
    for (const auto &t : s.tabs) o += "[" + t.title + ":" + std::to_string(t.focus) + ":" + dumpNode(*t.root) + "]";
    return o;
}
struct FakeProc : ProcSource {
    std::map<int, std::string> cwds;
    std::map<int, std::vector<std::string>> cmds;
    std::optional<std::string> cwd(int pid) const override {
        const auto it = cwds.find(pid);
        return it == cwds.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::optional<std::vector<std::string>> cmdline(int pid) const override {
        const auto it = cmds.find(pid);
        return it == cmds.end() ? std::nullopt : std::optional<std::vector<std::string>>(it->second);
    }
};
} // namespace

TEST_SUIT_BEGIN(SessionSnapshotSuite)

TEST_CASE("A snapshot survives serialize and parse exactly") {
    const auto s = sample();
    EXPECT(s.valid());
    const auto back = SessionSnapshot::parse(s.serialize());
    EXPECT(back.has_value());
    EXPECT_EQ(dump(*back), dump(s));
    EXPECT_EQ(back->serialize(), s.serialize());
}

TEST_CASE("A pane can name the daemon terminal it showed; only a 32 hex id is accepted") {
    const std::string id = "0123456789abcdef0123456789abcdef";
    SessionSnapshot s = sample();
    auto &leaf = *s.tabs[1].root;
    leaf.pane.terminal = id;
    EXPECT(s.valid());
    const auto text = s.serialize();
    const auto back = SessionSnapshot::parse(text);
    EXPECT(back.has_value());
    EXPECT_EQ(dump(*back), dump(s));
    EXPECT(back->tabs[1].root->pane.terminal == id);
    for (const std::string &bad : {std::string("xyz"), std::string(31, 'a'), std::string(33, 'a'),
                                  std::string("0123456789ABCDEF0123456789ABCDEF"), std::string("../../etc/passwd")}) {
        s.tabs[1].root->pane.terminal = bad;
        EXPECT(!s.valid());
        auto doc = text;
        doc.replace(doc.find(id), id.size(), bad);
        EXPECT(!SessionSnapshot::parse(doc).has_value());
    }
}

TEST_CASE("Names are plain file names") {
    EXPECT(SessionSnapshot::validName("work"));
    EXPECT(SessionSnapshot::validName("a.b_c-1"));
    for (const char *bad : {"", ".hidden", "..", "a/b", "a b", "a\n", "../x", "ö"}) EXPECT(!SessionSnapshot::validName(bad));
    EXPECT(!SessionSnapshot::validName(std::string(65, 'a')));
}

TEST_CASE("Anything but the exact schema is refused as a whole") {
    const auto good = sample().serialize();
    EXPECT(SessionSnapshot::parse(good).has_value());
    const auto bad = [&](const std::string &from, const std::string &to) -> bool {
        auto t = good;
        const auto at = t.find(from);
        if (at == std::string::npos) return false;
        t.replace(at, from.size(), to);
        return !SessionSnapshot::parse(t).has_value();
    };
    EXPECT(bad("\"version\":1", "\"version\":2"));
    EXPECT(bad("\"version\":1", "\"version\":1,\"extra\":1"));
    EXPECT(bad("\"name\":\"work\"", "\"name\":\"../x\""));
    EXPECT(bad("\"active\":1", "\"active\":2"));
    EXPECT(bad("\"active\":1", "\"active\":-1"));
    EXPECT(bad("\"ratio\":300", "\"ratio\":0"));
    EXPECT(bad("\"ratio\":300", "\"ratio\":1000"));
    EXPECT(bad("\"ratio\":300", "\"ratio\":3.5"));
    EXPECT(bad("\"split\":\"h\"", "\"split\":\"x\""));
    EXPECT(bad("\"focus\":2", "\"focus\":3")); // Only three panes.
    EXPECT(bad("\"cwd\":\"/src\"", "\"cwd\":\"src\""));          // Relative.
    EXPECT(bad("\"cwd\":\"/src\"", "\"cwd\":\"/s\\nrc\""));      // Control character.
    EXPECT(bad("\"make\"", "\"ma\\nke\""));                      // Newline in an argument.
    EXPECT(bad("\"make\"", "\"\""));                             // Empty program name.
    EXPECT(bad("\"title\":\"build\"", "\"title\":\"bu\\u202eild\"")); // Bidi override.
    EXPECT(bad("\"title\":\"build\"", "\"title\":\"bu\\u0007ild\""));
    EXPECT(bad("\"title\":\"build\"", "\"title\":\"bu\\ud800ild\""));  // Lone surrogate.
    EXPECT(bad("\"tabs\":[", "\"tabs\":[null,"));
    EXPECT(!SessionSnapshot::parse(good + " x").has_value());
    EXPECT(!SessionSnapshot::parse("").has_value());
    EXPECT(!SessionSnapshot::parse("[]").has_value());
    // Duplicate keys.
    EXPECT(!SessionSnapshot::parse("{\"version\":1,\"version\":1}").has_value());
    // Invalid UTF-8 in a string.
    auto t = good;
    t.replace(t.find("build"), 1, "\xff");
    EXPECT(!SessionSnapshot::parse(t).has_value());
}

TEST_CASE("Limits: file size, tabs, panes, depth, text") {
    auto s = sample();
    // Too many tabs.
    for (int i = 0; i < 20; ++i) {
        SavedTab t;
        t.root = SavedNode::leafOf(pane());
        s.tabs.push_back(std::move(t));
    }
    EXPECT(!s.valid());
    EXPECT(!SessionSnapshot::parse(s.serialize()).has_value());
    // Too many panes in one tab (9) and in the window (17).
    SessionSnapshot p;
    p.name = "p";
    SavedTab t;
    t.root = SavedNode::leafOf(pane());
    for (int i = 0; i < 8; ++i) t.root = SavedNode::split(SplitDirection::SideBySide, 500, std::move(t.root), SavedNode::leafOf(pane()));
    p.tabs.push_back(std::move(t));
    EXPECT(!p.valid());
    // Deep chain that stays inside the pane limits is still refused beyond depth 8.
    EXPECT(!SessionSnapshot::parse(std::string(2000, '[')).has_value());
    EXPECT(!SessionSnapshot::parse(std::string(SessionSnapshot::kMaxFile + 1, ' ')).has_value());
    // Text over the per-pane limit.
    SessionSnapshot q = sample();
    q.tabs[1].root->pane.text = std::string(SessionSnapshot::kMaxPaneText + 1, 'x');
    EXPECT(!q.valid());
    q.tabs[1].root->pane.text = SessionSnapshot::cleanText(std::string(SessionSnapshot::kMaxPaneText + 100, 'x'));
    EXPECT_EQ(q.tabs[1].root->pane.text.size(), SessionSnapshot::kMaxPaneText);
    EXPECT(q.valid());
}

TEST_CASE("Titles and text are cleaned: controls, bidi, invisibles go; characters are not cut in half") {
    EXPECT_EQ(SessionSnapshot::cleanTitle("a\x1b[31mb"), std::string("a[31mb"));
    EXPECT_EQ(SessionSnapshot::cleanTitle("x\xe2\x80\xae" "y"), std::string("xy")); // U+202E
    EXPECT_EQ(SessionSnapshot::cleanTitle("tab\n2"), std::string("tab2"));
    EXPECT_EQ(SessionSnapshot::cleanText("a\nb\x07\n"), std::string("a\nb\n"));
    const auto cut = SessionSnapshot::cleanTitle(std::string(SessionSnapshot::kMaxTitle - 1, 'a') + "\xc3\xb6\xc3\xb6");
    EXPECT_EQ(cut.size(), SessionSnapshot::kMaxTitle - 1); // The two-byte letter would not fit.
    EXPECT_EQ(SessionSnapshot::cleanTitle("\xff\xfe"), std::string());
}

TEST_CASE("Hostile files never crash the reader") {
    std::mt19937 rng(11);
    const auto good = sample().serialize();
    int accepted = 0;
    for (int i = 0; i < 4000; ++i) {
        auto t = good;
        for (int k = 0, n = 1 + int(rng() % 4); k < n; ++k) {
            const auto at = rng() % t.size();
            switch (rng() % 3) {
            case 0: t[at] = char(rng()); break;
            case 1: t.erase(at, 1 + rng() % 5); break;
            default: t.insert(at, 1, "{}[]\",:\\u0123"[rng() % 13]); break;
            }
            if (t.empty()) t = "x";
        }
        if (const auto r = SessionSnapshot::parse(t)) {
            ++accepted;
            EXPECT(r->valid()); // Whatever is accepted is valid and re-serializes.
            EXPECT(SessionSnapshot::parse(r->serialize()).has_value());
        }
    }
    EXPECT(accepted < 4000);
}

TEST_CASE("Probe: folder of the foreground program, command of a non-shell") {
    FakeProc p;
    p.cwds = {{100, "/home/a"}, {200, "/srv/app"}};
    p.cmds = {{100, {"-bash"}}, {200, {"vim", "notes.txt"}}};
    auto s = probePane(p, 100, 200);
    EXPECT_EQ(s.cwd, std::string("/srv/app"));
    EXPECT(s.busy);
    EXPECT_EQ(s.command.size(), std::size_t(2));
    // At the prompt: the shell, no command.
    s = probePane(p, 100, 100);
    EXPECT(!s.busy && s.command.empty());
    EXPECT_EQ(s.cwd, std::string("/home/a"));
    // Unknown foreground (0): the shell.
    s = probePane(p, 100, 0);
    EXPECT(!s.busy);
    // Foreground gone from /proc: the shell's folder, and something is running anyway.
    p.cwds.erase(200);
    p.cmds.erase(200);
    s = probePane(p, 100, 200);
    EXPECT_EQ(s.cwd, std::string("/home/a"));
    EXPECT(s.busy && s.command.empty());
}

TEST_CASE("Probe: hostile process data is not kept") {
    FakeProc p;
    p.cwds = {{1, "/ok"}, {2, "relative"}, {3, "/with\nnewline"}};
    p.cmds = {{1, {"-zsh"}}, {2, {"tool", "a\nb"}}, {3, {"x"}}};
    auto s = probePane(p, 1, 2);
    EXPECT_EQ(s.cwd, std::string("/ok")); // Relative folder refused, falls back to the shell.
    EXPECT(s.busy && s.command.empty());   // Newline in an argument: busy, but nothing to offer.
    s = probePane(p, 1, 3);
    EXPECT_EQ(s.cwd, std::string("/ok"));
    p.cmds[2] = std::vector<std::string>(40, "a");
    EXPECT(probePane(p, 1, 2).command.empty());
    p.cmds[2] = {"sh"};
    EXPECT(!probePane(p, 1, 2).busy); // A subshell is a shell.
}

TEST_CASE("Shell names") {
    for (const char *n : {"bash", "-bash", "/usr/bin/zsh", "fish", "sh", "dash"}) EXPECT(isShellName(n));
    for (const char *n : {"vim", "bashful", "ssh", "python3", "", "/bin/bash/x"}) EXPECT(!isShellName(n));
}

TEST_CASE("Quoting a saved command for a shell: one line, one word per argument") {
    EXPECT_EQ(commandLine({"vim", "a.txt"}), std::string("vim a.txt"));
    EXPECT_EQ(commandLine({"echo", "it's", "a b", "$HOME", ""}), std::string("echo 'it'\\''s' 'a b' '$HOME' ''"));
    EXPECT_EQ(commandLine({"ls", "-la", "/tmp/x;rm -rf ~"}), std::string("ls -la '/tmp/x;rm -rf ~'"));
    EXPECT_EQ(commandLine({"a", "b\nc"}), std::string()); // Refused whole.
    EXPECT_EQ(commandLine({}), std::string());
    // Every output is free of control characters and a single line.
    std::mt19937 rng(5);
    for (int i = 0; i < 500; ++i) {
        std::vector<std::string> argv = {"prog"};
        for (int k = 0; k < 3; ++k) {
            std::string a;
            for (int j = 0, n = int(rng() % 8); j < n; ++j) a += char(32 + rng() % 95);
            argv.push_back(a);
        }
        const auto line = commandLine(argv);
        EXPECT(!line.empty());
        for (const char c : line) EXPECT(!isControl(static_cast<unsigned char>(c)));
    }
}

TEST_SUIT_END
