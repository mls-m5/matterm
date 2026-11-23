#include "mls-unit-test/unittest.h"
#include "passthrough.h"

#include <memory>
#include <string>
#include <vector>

namespace {
using namespace frontend;
using namespace frontend::passthrough;
using Kind = Outcome::Kind;

struct FakeLink : TerminalLink {
    Events events;
    std::string sent;
    int columns = 0, rows = 0;
    std::optional<protocol::ErrorCode> error;
    void setEvents(Events e) override {
        events = std::move(e);
    }
    bool accept = true; // False: not connected yet.
    bool sendInput(std::string_view bytes) override {
        if (!accept) {
            return false;
        }
        sent.append(bytes);
        return true;
    }
    void resize(int c, int r, int, int) override {
        columns = c;
        rows = r;
    }
    void close() override {}
};

struct FakePane : ttymode::AttachedPane {
    FakeLink link;
    TerminalView v;
    bool replayFinished = false;
    int ticks = 0;
    explicit FakePane(int columns = 20, int rows = 4)
        : v(TerminalViewConfig{
              std::size_t(columns), std::size_t(rows), 100, {}}) {
        v.attach(link, [this] { return link.error; });
    }
    ~FakePane() override {
        v.detach();
    }
    TerminalView &view() override {
        return v;
    }
    void tick() override {
        ++ticks;
    }
    bool reattach() override {
        return false;
    }
    int pollFd() const override {
        return -1;
    }
    std::optional<std::int64_t> nextTickMs() const override {
        return std::nullopt;
    }
    bool replayDone() const override {
        return replayFinished;
    }
    void output(const std::string &text) {
        link.events.output(text);
    }
};

struct Rig {
    FakePane pane;
    bool restoredInAlt = false;
    std::unique_ptr<Session> session;
    std::int64_t now = 100;
    explicit Rig(std::size_t highWater = 1u << 20,
                 std::string attachLine = {},
                 bool fresh = false) {
        Session::Options options;
        options.fresh = fresh;
        options.title = "work";
        options.attachLine = std::move(attachLine);
        options.highWater = highWater;
        options.snapshot = [](const TerminalView &view) {
            // A stand-in for tty::snapshot: the first row of the model.
            std::string row;
            for (std::size_t x = 0; x < view.model().columns(); ++x) {
                const char32_t cp = view.model().cell(x, 0).codePoint;
                row += cp < 128 && cp > 31 ? char(cp) : ' ';
            }
            return "[SNAP:" + row.substr(0, row.find_last_not_of(' ') + 1) +
                   "]";
        };
        options.restore = [this](bool alt) {
            restoredInAlt = alt;
            return std::string(alt ? "[RESTORE-ALT]" : "[RESTORE]");
        };
        session = std::make_unique<Session>(pane, std::move(options), now);
    }
    void tick(std::int64_t advance = 10) {
        now += advance;
        session->tick(now);
    }
};
} // namespace

TEST_SUIT_BEGIN(Passthrough)

TEST_CASE("Until the replay is in, nothing is written; then one snapshot, then "
          "the bytes unchanged") {
    Rig rig(1u << 20, "keys: Ctrl+B d");
    rig.pane.output("history\r\n");
    rig.tick();
    EXPECT(rig.session->hostOut().empty());
    EXPECT(!rig.session->live());
    rig.pane.replayFinished = true;
    rig.tick();
    EXPECT(rig.session->live());
    EXPECT_EQ(rig.session->hostOut(),
              std::string("keys: Ctrl+B d\r\n[SNAP:history]"));
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output("\x1b[31mred\x1b[0m\r\n");
    rig.pane.output("more");
    EXPECT_EQ(rig.session->hostOut(),
              std::string("\x1b[31mred\x1b[0m\r\nmore"));
}

TEST_CASE("Bytes that come with the replay but after the snapshot's point are "
          "in the snapshot, not twice") {
    Rig rig;
    rig.pane.output("a");
    rig.pane.output(
        "b"); // Still part of what the model got before the screen was shown.
    rig.pane.replayFinished = true;
    rig.tick();
    EXPECT_EQ(rig.session->hostOut(), std::string("[SNAP:ab]"));
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output("c");
    EXPECT_EQ(rig.session->hostOut(), std::string("c"));
}

TEST_CASE("A terminal this client just made is live from the first byte: no "
          "snapshot, the first bytes go through") {
    Rig rig(1u << 20, "keys", true);
    EXPECT(rig.session->live());
    EXPECT_EQ(rig.session->hostOut(), std::string("keys\r\n"));
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output(
        "\x1b[c"); // What neovim asks first: it must reach the host terminal.
    EXPECT_EQ(rig.session->hostOut(), std::string("\x1b[c"));
    rig.tick();
    EXPECT(rig.session->hostOut().find("SNAP") ==
           std::string::npos); // Never a snapshot.
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output("more");
    EXPECT_EQ(rig.session->hostOut(), std::string("more"));
}

TEST_CASE(
    "Input typed before the connection runs is kept and sent when it does") {
    Rig rig(1u << 20, {}, true);
    rig.pane.link.accept = false;
    rig.session->input("ls");
    rig.tick();
    EXPECT(rig.pane.link.sent.empty());
    rig.pane.link.accept = true;
    rig.tick();
    EXPECT_EQ(rig.pane.link.sent, std::string("ls"));
    rig.tick();
    EXPECT_EQ(rig.pane.link.sent, std::string("ls")); // Not twice.
}

TEST_CASE(
    "Echo measures forwarded input, while wire counting belongs to transport") {
    perf::Counters counters;
    perf::EchoMeter echo;
    double now = 0;
    // A session of its own, with the meters.
    Session::Options options;
    options.fresh = true;
    options.counters = &counters;
    options.echo = &echo;
    options.clockMs = [&] { return now; };
    FakePane pane;
    Session session(pane, options, 0);
    pane.output("0123456789");
    EXPECT_EQ(counters.bytesIn, std::uint64_t(0));
    now = 100;
    session.input("a");
    EXPECT_EQ(counters.bytesOut, std::uint64_t(0));
    now = 118;
    pane.output("a");
    EXPECT_EQ(echo.samples().summary().count, std::size_t(1));
    EXPECT(std::abs(echo.samples().summary().avg - 18) < 1e-9);
    EXPECT_EQ(counters.messagesIn, std::uint64_t(0));
    EXPECT(counters.parseMs >= 0);
}

TEST_CASE("The screen is shown anyway when the replay never ends") {
    Rig rig;
    rig.pane.output("x");
    rig.tick(1000);
    EXPECT(!rig.session->live());
    rig.tick(6000);
    EXPECT(rig.session->live());
    EXPECT(rig.session->hostOut().find("[SNAP:x]") != std::string::npos);
}

TEST_CASE("Input goes to the program as it came, also what the host sends for "
          "the modes the program asked") {
    Rig rig;
    rig.pane.replayFinished = true;
    rig.tick();
    rig.session->input("ls\r");
    rig.session->input("\x1b[A\x1bOB\x1b[<0;3;2M\x1b[200~paste\x1b[201~");
    EXPECT_EQ(
        rig.pane.link.sent,
        std::string("ls\r\x1b[A\x1bOB\x1b[<0;3;2M\x1b[200~paste\x1b[201~"));
}

TEST_CASE("Input typed before the screen is shown waits for it") {
    Rig rig;
    rig.session->input("early");
    EXPECT(rig.pane.link.sent.empty());
    rig.pane.replayFinished = true;
    rig.tick();
    EXPECT_EQ(rig.pane.link.sent, std::string("early"));
}

TEST_CASE("Ctrl+B d, l and c end the session in their ways, nothing of them "
          "goes to the program") {
    for (const auto &[keys, kind] :
         std::vector<std::pair<std::string, Kind>>{{"\x02"
                                                    "d",
                                                    Kind::Leave},
                                                   {"\x02"
                                                    "l",
                                                    Kind::List},
                                                   {"\x02"
                                                    "c",
                                                    Kind::New},
                                                   {"\x1d", Kind::List}}) {
        Rig rig;
        rig.pane.replayFinished = true;
        rig.tick();
        rig.session->input("a" + keys + "b");
        EXPECT(rig.session->outcome().has_value());
        EXPECT(rig.session->outcome()->kind == kind);
        EXPECT_EQ(
            rig.pane.link.sent,
            std::string("a")); // The rest after the command is not sent either.
    }
}

TEST_CASE("Ctrl+B twice sends one Ctrl+B; another key after Ctrl+B only "
          "cancels; a split prefix works") {
    Rig rig;
    rig.pane.replayFinished = true;
    rig.tick();
    rig.session->input("\x02\x02");
    EXPECT_EQ(rig.pane.link.sent, std::string("\x02"));
    rig.session->input("\x02x");
    EXPECT_EQ(rig.pane.link.sent, std::string("\x02"));
    rig.session->input("\x02");
    EXPECT(!rig.session->outcome());
    rig.session->input("d");
    EXPECT(rig.session->outcome() &&
           rig.session->outcome()->kind == Kind::Leave);
}

TEST_CASE(
    "The program ending is an outcome after its last words are in hostOut") {
    Rig rig;
    rig.pane.replayFinished = true;
    rig.tick();
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output("bye\r\n");
    rig.pane.link.events.exited(3, 0);
    rig.tick();
    EXPECT(rig.session->hostOut().find("bye") != std::string::npos);
    EXPECT(rig.session->outcome() &&
           rig.session->outcome()->kind == Kind::Ended);
    EXPECT_EQ(rig.session->outcome()->exitCode, 3);
    EXPECT(rig.session->outcome()->message.find(
               "The program in work ended (exit code 3)") != std::string::npos);
}

TEST_CASE("Taken over, lost and gone are outcomes with a plain line, and a "
          "gone terminal gets no snapshot") {
    Rig kicked;
    kicked.pane.replayFinished = true;
    kicked.tick();
    kicked.pane.link.error = protocol::ErrorCode::Replaced;
    kicked.pane.link.events.closed("session taken over");
    kicked.tick();
    EXPECT(kicked.session->outcome() &&
           kicked.session->outcome()->kind == Kind::TakenOver);
    EXPECT(kicked.session->outcome()->message.find("taken over") !=
           std::string::npos);
    Rig lost;
    lost.pane.replayFinished = true;
    lost.tick();
    lost.pane.link.events.closed("connection lost");
    lost.tick();
    EXPECT(lost.session->outcome() &&
           lost.session->outcome()->kind == Kind::Lost);
    Rig gone;
    gone.pane.link.error = protocol::ErrorCode::ResumeUnsupported;
    gone.pane.link.events.closed("no such session");
    gone.tick();
    EXPECT(gone.session->outcome() &&
           gone.session->outcome()->kind == Kind::Gone);
    EXPECT(gone.session->hostOut().find("SNAP") == std::string::npos);
}

TEST_CASE("Too much waiting for the host: the daemon is not read; nothing is "
          "dropped") {
    Rig rig(100);
    rig.pane.replayFinished = true;
    rig.tick();
    rig.session->consumed(rig.session->hostOut().size());
    EXPECT(rig.session->wantsRead());
    rig.pane.output(std::string(150, 'x'));
    EXPECT(!rig.session->wantsRead());
    const int ticks = rig.pane.ticks;
    rig.tick();
    EXPECT_EQ(rig.pane.ticks, ticks); // Not served while backed up.
    EXPECT_EQ(rig.session->hostOut().size(), std::size_t(150));
    rig.session->consumed(100);
    EXPECT(rig.session->wantsRead());
    rig.tick();
    EXPECT(rig.pane.ticks > ticks);
    EXPECT_EQ(rig.session->hostOut(), std::string(50, 'x'));
}

TEST_CASE("A resize of the host goes to the daemon, the model follows") {
    Rig rig;
    rig.pane.replayFinished = true;
    rig.tick();
    rig.session->resized(100, 30);
    EXPECT_EQ(rig.pane.link.columns, 100);
    EXPECT_EQ(rig.pane.link.rows, 30);
    EXPECT_EQ(rig.pane.v.model().columns(), std::size_t(100));
}

TEST_CASE("The restore knows whether the program is in the alternate screen") {
    Rig plain;
    plain.pane.replayFinished = true;
    plain.tick();
    EXPECT_EQ(plain.session->restore(), std::string("[RESTORE]"));
    Rig alt;
    alt.pane.output("\x1b[?1049h");
    alt.pane.replayFinished = true;
    alt.tick();
    EXPECT_EQ(alt.session->restore(), std::string("[RESTORE-ALT]"));
    EXPECT(alt.restoredInAlt);
}

TEST_CASE("Fresh startup queries are forwarded from the first byte") {
    Rig rig(1u << 20, "keys", true);
    EXPECT(rig.session->live());
    EXPECT_EQ(rig.session->hostOut(), std::string("keys\r\n"));
    rig.session->consumed(rig.session->hostOut().size());
    rig.pane.output("\x1b[c");
    EXPECT_EQ(rig.session->hostOut(), std::string("\x1b[c"));
    rig.tick();
    EXPECT(rig.session->hostOut().find("SNAP") == std::string::npos);
}

TEST_CASE("Fresh input waits for the connection instead of being lost") {
    Rig rig(1u << 20, {}, true);
    rig.pane.link.accept = false;
    rig.session->input("ls");
    rig.tick();
    EXPECT(rig.pane.link.sent.empty());
    rig.pane.link.accept = true;
    rig.tick();
    EXPECT_EQ(rig.pane.link.sent, std::string("ls"));
    rig.tick();
    EXPECT_EQ(rig.pane.link.sent, std::string("ls"));
}

TEST_SUIT_END
