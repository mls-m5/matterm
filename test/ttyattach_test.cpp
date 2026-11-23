#include "mls-unit-test/unittest.h"
#include "ttyattach.h"

#include <memory>
#include <string>
#include <vector>

namespace {
using namespace frontend;
using namespace frontend::ttymode;
using State = TerminalView::State;

struct Shot {
    int columns = 0, rows = 0;
    std::vector<std::string> text;
    std::string bytes; // What the terminal is sent for this picture (cursor
                       // moves, shape).
};

struct FakeHost : HostScreen {
    ::tty::Screen screen_{80, 8};
    std::vector<Shot> shown;
    std::vector<HostInputModes> modes;
    void setSize(int columns, int rows) {
        screen_.resize(columns, rows);
    }
    ::tty::Screen &screen() override {
        return screen_;
    }
    void present() override {
        Shot shot;
        shot.columns = screen_.columns();
        shot.rows = screen_.rows();
        for (int y = 0; y < shot.rows; ++y) {
            std::string out;
            for (int x = 0; x < shot.columns; ++x) {
                const char32_t cp = screen_.canvas().at(x, y).codePoint;
                out += cp < 128 ? char(cp) : '?';
            }
            while (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
            shot.text.push_back(out);
        }
        shot.bytes = screen_.present();
        shown.push_back(std::move(shot));
    }
    void setInputModes(const HostInputModes &m) override {
        modes.push_back(m);
    }
    std::string row(int y) const {
        return shown.back().text[std::size_t(y)];
    }
    bool shows(const std::string &text) const {
        for (const std::string &line : shown.back().text) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

// A terminal that is only a record of what was sent to it and a way to make
// things happen to it.
struct FakeLink : TerminalLink {
    Events events;
    std::string sent;
    int columns = 0, rows = 0;
    std::optional<protocol::ErrorCode> error;
    void setEvents(Events e) override {
        events = std::move(e);
    }
    bool sendInput(std::string_view bytes) override {
        sent.append(bytes);
        return true;
    }
    void resize(int c, int r, int, int) override {
        columns = c;
        rows = r;
    }
    void close() override {}
    void output(const std::string &text) {
        events.output(text);
    }
};

struct FakePane : AttachedPane {
    FakeLink link;
    TerminalView v;
    int reattaches = 0;
    int ticks = 0;
    explicit FakePane(int columns, int rows)
        : v(TerminalViewConfig{
              std::size_t(columns), std::size_t(rows), 100, {}}) {
        attach();
    }
    ~FakePane() override {
        v.detach();
    }
    void attach() {
        link.error.reset();
        v.attach(link, [this] { return link.error; });
    }
    TerminalView &view() override {
        return v;
    }
    void tick() override {
        ++ticks;
    }
    bool reattach() override {
        ++reattaches;
        attach();
        return true;
    }
    int pollFd() const override {
        return -1;
    }
    std::optional<std::int64_t> nextTickMs() const override {
        return std::nullopt;
    }
    void takenOver() {
        link.error = protocol::ErrorCode::Replaced;
        link.events.closed("session taken over");
    }
};

struct FakeSource : SessionSource {
    std::vector<ListEntry> entries;
    std::string listError;
    std::string openError;
    FakePane *last = nullptr; // Valid while the mode holds it.
    int lists = 0;
    int opened = 0;
    std::string openedId;
    std::vector<ListEntry> list(std::string &error) override {
        ++lists;
        error = listError;
        return entries;
    }
    std::unique_ptr<AttachedPane> open(const std::string &id,
                                       int columns,
                                       int rows,
                                       std::string &error) override {
        if (!openError.empty()) {
            error = openError;
            return nullptr;
        }
        ++opened;
        openedId = id;
        auto pane = std::make_unique<FakePane>(columns, rows);
        last = pane.get();
        return pane;
    }
};

ListEntry entry(const std::string &id,
                const std::string &title,
                bool inUse = false) {
    ListEntry e;
    e.id = id;
    e.title = title;
    e.what = "shell";
    e.inUse = inUse;
    return e;
}

struct Rig {
    FakeHost host;
    FakeSource source;
    std::unique_ptr<AttachTty> mode;
    std::int64_t now = 1000;
    explicit Rig(std::vector<ListEntry> entries = {entry("a", "alpha"),
                                                   entry("b", "beta", true)}) {
        source.entries = std::move(entries);
        mode = std::make_unique<AttachTty>(host, source);
        mode->start(now);
    }
    void type(const std::string &bytes) {
        now += 10;
        mode->input(bytes, now);
    }
    void enterPane() {
        type("\r"); // Open the first row.
    }
};
} // namespace

TEST_SUIT_BEGIN(TtyAttach)

TEST_CASE("It starts with the list, also when there is only one terminal") {
    Rig one({entry("a", "alpha")});
    EXPECT(!one.mode->inPane());
    EXPECT(one.host.shows("matterm: running terminals"));
    EXPECT(one.host.shows("alpha"));
    EXPECT_EQ(one.source.opened, 0);
}

TEST_CASE(
    "Choosing a row takes the terminal over at the size of the host screen") {
    Rig rig;
    rig.type("\x1b[B"); // Down: the second one, which another client has.
    rig.type("\r");
    EXPECT(rig.mode->inPane());
    EXPECT_EQ(rig.source.openedId, std::string("b"));
    EXPECT_EQ(rig.source.last->v.model().columns(), std::size_t(80));
    EXPECT_EQ(rig.source.last->v.model().rows(), std::size_t(8));
}

TEST_CASE("q, Escape and Ctrl+C in the list end the mode, nothing is opened") {
    for (const std::string key : {"q", "\x03", "\x1d"}) {
        Rig rig;
        rig.type(key);
        EXPECT(rig.mode->finished());
        EXPECT_EQ(rig.source.opened, 0);
    }
    Rig rig;
    rig.type("\x1b");
    EXPECT(!rig.mode->finished()); // Might be the start of a sequence.
    rig.mode->quiet(rig.now + 50);
    EXPECT(rig.mode->finished());
}

TEST_CASE("Typed characters in the list are keystrokes one by one") {
    Rig rig;
    rig.type("j\r"); // Down, Enter in one read.
    EXPECT_EQ(rig.source.openedId, std::string("b"));
    Rig again;
    again.type("jk2");
    EXPECT(again.mode->list().index() == 1);
}

TEST_CASE(
    "A terminal that cannot be opened stays in the list with the reason") {
    Rig rig;
    rig.source.openError = "the terminal is gone";
    rig.enterPane();
    EXPECT(!rig.mode->inPane());
    EXPECT(rig.host.shows("the terminal is gone"));
}

TEST_CASE("An empty or failing daemon says so") {
    Rig rig(std::vector<ListEntry>{});
    rig.source.listError = "the daemon is not running";
    rig.type("r");
    EXPECT_EQ(rig.source.lists, 2);
    EXPECT(rig.host.shows("the daemon is not running"));
    rig.type("\r"); // Nothing to open.
    EXPECT(!rig.mode->inPane());
}

TEST_CASE("The pane shows the program's screen and sends the keys on") {
    Rig rig;
    rig.enterPane();
    rig.source.last->link.output("hello\r\nworld");
    rig.mode->tick(rig.now);
    EXPECT_EQ(rig.host.row(0), std::string("hello"));
    EXPECT_EQ(rig.host.row(1), std::string("world"));
    // The cursor is after "world": row 2, column 6 (1-based) and shown.
    EXPECT(rig.host.shown.back().bytes.find("\x1b[2;6H") != std::string::npos);
    EXPECT(rig.host.shown.back().bytes.find("\x1b[?25h") != std::string::npos);
    rig.type("ls\r");
    EXPECT_EQ(rig.source.last->link.sent, std::string("ls\r"));
    rig.type("\x1b[A");
    EXPECT_EQ(rig.source.last->link.sent, std::string("ls\r\x1b[A"));
    rig.source.last->link.output("\x1b[?1h"); // Application cursor keys.
    rig.type("\x1b[A");
    EXPECT_EQ(rig.source.last->link.sent, std::string("ls\r\x1b[A\x1bOA"));
}

TEST_CASE(
    "A paste goes in one piece, in brackets only when the program asked") {
    Rig rig;
    rig.enterPane();
    rig.type("\x1b[200~a\nb\x1b[201~");
    EXPECT_EQ(rig.source.last->link.sent, std::string("a\rb"));
    rig.source.last->link.output("\x1b[?2004h");
    rig.type("\x1b[200~c\x1b[201~");
    EXPECT_EQ(rig.source.last->link.sent,
              std::string("a\rb\x1b[200~c\x1b[201~"));
}

TEST_CASE(
    "The host terminal reports the mouse only while the program wants it") {
    Rig rig;
    rig.enterPane();
    EXPECT(!rig.host.modes.back().mouse);
    rig.source.last->link.output("\x1b[?1000h\x1b[?1006h");
    rig.mode->tick(rig.now);
    EXPECT(rig.host.modes.back().mouse);
    rig.type("\x1b[<0;3;2M");
    EXPECT_EQ(rig.source.last->link.sent, std::string("\x1b[<0;3;2M"));
    rig.type("\x1d"); // Back to the list: no mouse there.
    EXPECT(!rig.host.modes.back().mouse);
}

TEST_CASE("Resizing the host terminal resizes the terminal in the daemon") {
    Rig rig;
    rig.enterPane();
    rig.host.setSize(60, 20);
    rig.mode->resized(rig.now + 5);
    EXPECT_EQ(rig.source.last->v.model().columns(), std::size_t(60));
    EXPECT_EQ(rig.source.last->link.columns, 60);
    EXPECT_EQ(rig.source.last->link.rows, 20);
    EXPECT_EQ(rig.host.shown.back().columns, 60);
    EXPECT_EQ(rig.host.shown.back().rows, 20);
}

TEST_CASE(
    "The leave key goes back to the list and the terminal keeps running") {
    Rig rig;
    rig.enterPane();
    rig.type("a\x1d");
    EXPECT(!rig.mode->inPane());
    EXPECT(!rig.mode->finished());
    EXPECT_EQ(rig.source.lists, 2); // The list is read again.
    EXPECT(rig.host.shows("Left alpha; it keeps running"));
    rig.type("\r"); // And it can be opened again.
    EXPECT(rig.mode->inPane());
    EXPECT_EQ(rig.source.opened, 2);
}

TEST_CASE("The first time, a hint names the leave key; it goes with the first "
          "key or after a while") {
    Rig rig;
    rig.enterPane();
    EXPECT(rig.host.shows("Ctrl+] leaves to the list"));
    rig.type("x");
    EXPECT(!rig.host.shows("Ctrl+] leaves"));
    // Again: no hint the second time in this run.
    rig.type("\x1d");
    rig.enterPane();
    EXPECT(!rig.host.shows("Ctrl+] leaves"));
    // And it fades by itself.
    Rig slow;
    slow.enterPane();
    EXPECT(slow.host.shows("Ctrl+] leaves"));
    EXPECT(slow.mode->timeoutMs(slow.now) > 0);
    slow.mode->tick(slow.now + 9000);
    EXPECT(!slow.host.shows("Ctrl+] leaves"));
}

TEST_CASE("Taken over by another client: a line says so, Enter takes it back, "
          "keys are not sent") {
    Rig rig;
    rig.enterPane();
    rig.source.last->takenOver();
    rig.mode->tick(rig.now);
    EXPECT(rig.source.last->v.state() == State::Replaced);
    EXPECT(rig.host.shows("Taken over by another client"));
    EXPECT(rig.host.shows("Enter: take it back"));
    EXPECT(!rig.host.modes.back().mouse);
    rig.type("abc");
    EXPECT_EQ(rig.source.last->link.sent, std::string());
    rig.type("\r");
    EXPECT_EQ(rig.source.last->reattaches, 1);
    EXPECT(rig.source.last->v.state() == State::Live);
    EXPECT(!rig.host.shows("Taken over"));
    rig.type("x");
    EXPECT_EQ(rig.source.last->link.sent, std::string("x"));
}

TEST_CASE(
    "On a narrow screen the line gets shorter but still says what Enter does") {
    Rig rig;
    rig.host.setSize(30, 8);
    rig.enterPane();
    rig.source.last->takenOver();
    rig.mode->tick(rig.now);
    EXPECT(rig.host.shows("Taken over. Enter: take back"));
}

TEST_CASE("After taking it over, the other client can be kicked in turn: the "
          "line comes back") {
    Rig rig;
    rig.enterPane();
    rig.source.last->takenOver();
    rig.type("\r");
    rig.source.last->takenOver();
    rig.mode->tick(rig.now);
    EXPECT(rig.host.shows("Taken over by another client"));
}

TEST_CASE("A lost connection offers to connect again") {
    Rig rig;
    rig.enterPane();
    rig.source.last->link.events.closed("connection lost");
    rig.mode->tick(rig.now);
    EXPECT(rig.host.shows("Connection lost"));
    rig.type("\r");
    EXPECT_EQ(rig.source.last->reattaches, 1);
}

TEST_CASE("A terminal that ended or is gone goes back to the list with Enter") {
    Rig rig;
    rig.enterPane();
    rig.source.last->link.events.exited(0, 0);
    rig.mode->tick(rig.now);
    EXPECT(rig.host.shows("The program ended"));
    rig.type("\r");
    EXPECT(!rig.mode->inPane());
}

TEST_CASE("The leave key works in every state") {
    Rig rig;
    rig.enterPane();
    rig.source.last->takenOver();
    rig.mode->tick(rig.now);
    rig.type("\x1d");
    EXPECT(!rig.mode->inPane());
}

TEST_CASE("Nothing is drawn twice when nothing changed") {
    Rig rig;
    const auto shown = rig.host.shown.size();
    rig.mode->tick(rig.now + 10);
    rig.mode->tick(rig.now + 20);
    EXPECT_EQ(rig.host.shown.size(), shown);
}

TEST_CASE("The mode waits for a lone Escape only briefly") {
    Rig rig;
    EXPECT_EQ(rig.mode->timeoutMs(rig.now), -1);
    rig.type("\x1b");
    const int wait = rig.mode->timeoutMs(rig.now);
    EXPECT(wait > 0 && wait <= 100);
}

TEST_SUIT_END
