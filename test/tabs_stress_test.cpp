// Real shells in real panes: what happens to processes, descriptors and sizes when
// panes are closed, die hidden, are created and closed in a loop, and a divider is
// dragged hard (docs/TABS_PLAN.md, lifecycle review).
#include "mls-unit-test/unittest.h"
#include "ptylink.h"
#include "terminalview.h"
#include "workspaceview.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <dirent.h>
#include <fstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace frontend;

namespace {

struct LivePane {
    PtyLink link; // before the view: the view borrows it
    TerminalView v;
    explicit LivePane(const std::vector<std::string> &command)
        : link(command, 80, 24, "xterm-256color")
        , v() {
        v.attach(link);
    }
    TerminalView &view() { return v; }
    const TerminalView &view() const { return v; }
};

using Space = Workspace<LivePane>;

std::vector<std::string> sh(const std::string &script) {
    return {"/bin/sh", "-c", script};
}

int openFds() {
    int n = 0;
    if (DIR *d = ::opendir("/proc/self/fd")) {
        while (::readdir(d) != nullptr) ++n;
        ::closedir(d);
    }
    return n - 3; // ".", ".." and the directory's own descriptor
}

// Serve every process like the app's loop: pump all panes, close those that ended.
// Returns the number of closed panes.
int serve(Space &w) {
    std::vector<PaneId> ended;
    w.forEachPane([&](PaneId id, LivePane &p, bool) {
        p.link.pump();
        if (p.link.finished()) ended.push_back(id);
    });
    for (PaneId id : ended) w.closePaneAnywhere(id);
    return int(ended.size());
}

template <class Condition>
bool waitFor(Space &w, int ms, Condition done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        serve(w);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

long readNumber(const std::string &path) {
    std::ifstream in(path);
    long n = 0;
    in >> n;
    return n;
}

bool alive(long pid) {
    // A zombie still answers kill(pid, 0): look at the state letter too.
    if (pid <= 0 || ::kill(static_cast<pid_t>(pid), 0) != 0) return false;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

bool noChildren() {
    int status = 0;
    return ::waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD;
}

LayoutSpec spec() {
    LayoutSpec s;
    s.area = {0, 0, 2400, 1200};
    s.cellWidth = 8;
    s.cellHeight = 16;
    s.dividerThickness = 12;
    return s;
}

// What the screen shows now (the viewport follows the tail; scrollback is not read).
std::string screenText(TerminalModel &m) {
    m.followTail();
    m.advanceViewport(10);
    const std::size_t first = m.firstRetainedRow() + m.viewport().firstRow();
    const std::size_t rows = m.viewport().rowCount();
    if (rows == 0) return "";
    return m.copyText({first, 0}, {first + rows - 1, m.columns() - 1}, 1 << 20).value_or("");
}

std::string tmp(const char *name) {
    return "/tmp/matterm-stress-" + std::to_string(::getpid()) + "-" + name;
}

} // namespace

TEST_SUIT_BEGIN(TabsStress)

TEST_CASE("Closing a pane ends its process like closing a terminal window, with no zombie") {
    Space w([] { return std::make_unique<LivePane>(sh("echo $$ > " + tmp("a") + "; exec sleep 60")); });
    w.setLayoutSpec(spec());
    w.apply(Command::NewTab);
    EXPECT(waitFor(w, 3000, [] { return readNumber(tmp("a")) > 0; }));
    const long pid = readNumber(tmp("a"));
    EXPECT(alive(pid));
    const auto start = std::chrono::steady_clock::now();
    EXPECT(w.apply(Command::ClosePane) == Outcome::Empty);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT(!alive(pid));
    EXPECT(noChildren());
    EXPECT(ms < 500); // SIGHUP is enough for a normal program
    ::unlink(tmp("a").c_str());
}

TEST_CASE("A program that ignores SIGHUP is killed after a short grace, the GUI is not blocked for long") {
    Space w([] {
        return std::make_unique<LivePane>(sh("trap '' HUP; echo $$ > " + tmp("b") + "; while :; do sleep 1; done"));
    });
    w.setLayoutSpec(spec());
    w.apply(Command::NewTab);
    EXPECT(waitFor(w, 3000, [] { return readNumber(tmp("b")) > 0; }));
    const long pid = readNumber(tmp("b"));
    const auto start = std::chrono::steady_clock::now();
    w.apply(Command::CloseTab);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT(!alive(pid));
    EXPECT(noChildren());
    EXPECT(ms < 1000);
    ::unlink(tmp("b").c_str());
}

TEST_CASE("Background jobs of the shell get the hangup too") {
    Space w([] {
        return std::make_unique<LivePane>(sh("sleep 60 & echo $! > " + tmp("c") + "; wait"));
    });
    w.setLayoutSpec(spec());
    w.apply(Command::NewTab);
    EXPECT(waitFor(w, 3000, [] { return readNumber(tmp("c")) > 0; }));
    const long job = readNumber(tmp("c"));
    EXPECT(alive(job));
    w.apply(Command::CloseTab);
    // The job is reparented; allow init a moment to reap it.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (alive(job) && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(!alive(job));
    ::unlink(tmp("c").c_str());
}

TEST_CASE("A process that dies while its tab is hidden closes its pane and leaves no zombie") {
    int made = 0;
    Space w([&made] {
        // The first pane lives; the second exits at once.
        return std::make_unique<LivePane>(++made == 1 ? sh("exec sleep 60") : sh("exit 3"));
    });
    w.setLayoutSpec(spec());
    w.apply(Command::NewTab);
    w.apply(Command::NewTab); // active; the first is hidden
    w.apply(Command::SelectTab, 0);
    // The dead one is in the hidden tab now; wait for the loop to notice it.
    EXPECT(waitFor(w, 3000, [&w] { return w.tabCount() == 1; }));
    EXPECT_EQ(w.paneCount(), std::size_t(1));
    EXPECT_EQ(w.activeIndex(), std::size_t(0));
    w.apply(Command::CloseTab);
    EXPECT(noChildren());
}

TEST_CASE("Hidden and parked panes keep being served") {
    Space w([] { return std::make_unique<LivePane>(sh("i=0; while :; do i=$((i+1)); echo line$i; sleep 0.01; done")); });
    w.setLayoutSpec(spec());
    w.apply(Command::NewTab);
    const PaneId first = w.focusedPaneId();
    w.apply(Command::NewTab);
    w.apply(Command::SplitSideBySide);
    w.park(w.focusedPaneId());
    const PaneId parked = w.parkedIds()[0];
    auto lines = [&](PaneId id) { return screenText(w.pane(id)->view().model()); };
    waitFor(w, 300, [] { return false; });
    const std::string a = lines(first), b = lines(parked);
    waitFor(w, 600, [] { return false; });
    EXPECT(lines(first) != a); // hidden tab
    EXPECT(lines(parked) != b); // parked
}

TEST_CASE("Creating and closing tabs and panes in a loop leaks no descriptor, process or pane") {
    Space w([] { return std::make_unique<LivePane>(sh("exec sleep 60")); });
    w.setLayoutSpec(spec());
    const int before = openFds();
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < 40; ++round) {
        w.apply(Command::NewTab);
        w.apply(Command::SplitSideBySide);
        w.apply(Command::SplitStacked);
        w.apply(Command::SplitSideBySide);
        EXPECT_EQ(w.paneCount(), std::size_t(4));
        // Alternate how they end: whole tab, or pane by pane.
        if (round % 2 == 0) {
            w.apply(Command::CloseTab);
        }
        else {
            for (int i = 0; i < 4; ++i) w.apply(Command::ClosePane);
        }
        EXPECT_EQ(w.tabCount(), std::size_t(0));
        EXPECT_EQ(w.paneCount(), std::size_t(0));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(openFds(), before);
    EXPECT(noChildren());
    EXPECT(ms < 20000);
}

TEST_CASE("A divider drag storm: sizes are throttled and every program ends with the right size") {
    Space w([] { return std::make_unique<LivePane>(sh("exec sh")); });
    WorkspaceView<LivePane> view(w);
    view.setWindow({2400, 1200, 8, 16});
    view.key({'t', true, true, false}, 0);
    for (int i = 0; i < 3; ++i) view.key({'e', true, true, false}, 0);
    for (int i = 0; i < 3; ++i) view.key({'o', true, true, false}, 0);
    EXPECT_EQ(w.paneCount(), std::size_t(7));
    std::int64_t now = 0;
    view.sync(now, true);
    const FramePlan p = view.plan();
    EXPECT(!p.dividers.empty());
    // Grab the first divider and shake it, 1000 pointer events per second for 3 s.
    const PixelRect g = p.dividers[0].rect;
    const int y = g.y + g.h / 2;
    MouseInput in;
    in.type = MouseInput::Type::Down;
    in.button = MouseInput::Button::Left;
    in.x = g.x + g.w * 3 / 4;
    in.y = y;
    view.mouse(in, now);
    int syncs = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3000; ++i) {
        ++now;
        in.type = MouseInput::Type::Move;
        in.x = 200 + (i * 37) % 1600;
        view.mouse(in, now);
        if (view.sync(now)) ++syncs;
        if (i % 20 == 0) serve(w);
    }
    in.type = MouseInput::Type::Up;
    view.mouse(in, ++now);
    view.sync(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT(syncs <= 3000 / 40 + 5); // 40 ms between sizes, not one per event
    EXPECT(ms < 15000);
    // Every program must now agree with the plan: ask the kernel through stty.
    const FramePlan end = view.plan();
    for (const auto &pp : end.panes) w.pane(pp.id)->view().sendInput("stty size\n");
    for (const auto &pp : end.panes) {
        const std::string want = std::to_string(pp.rows) + " " + std::to_string(pp.columns);
        EXPECT(waitFor(w, 5000, [&] { return screenText(w.pane(pp.id)->view().model()).find(want) != std::string::npos; }));
    }
    for (const auto &pp : end.panes) {
        EXPECT_EQ(w.pane(pp.id)->view().model().columns(), std::size_t(pp.columns));
    }
}

TEST_SUIT_END
