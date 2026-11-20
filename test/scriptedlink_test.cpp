#include "mls-unit-test/unittest.h"
#include "scriptedlink.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <string>
#include <vector>

using std::chrono::milliseconds;

namespace {
// A link, a fake clock, and what the link reported.
struct Harness {
    explicit Harness(Script script = {}, bool simulator = false) {
        ScriptedLink::Config config;
        config.clock = [this] { return protocol::Time(now); };
        config.columns = 40;
        config.rows = 10;
        if (simulator) {
            link = ScriptedLink::simulator(config);
        }
        else {
            link = std::make_unique<ScriptedLink>(config, std::move(script));
        }
        TerminalLink::Events events;
        events.output = [this](std::string_view bytes) {
            output.append(bytes);
            ++outputCalls;
        };
        events.exited = [this](int code, int) { exits.push_back(code); };
        events.closed = [this](std::string_view) { ++closes; };
        link->setEvents(std::move(events));
    }
    void at(long long time) {
        now = milliseconds(time);
        link->tick();
    }
    std::string take() {
        std::string result;
        result.swap(output);
        return result;
    }

    milliseconds now{0};
    std::unique_ptr<ScriptedLink> link;
    std::string output;
    int outputCalls = 0;
    std::vector<int> exits;
    int closes = 0;
};

// The simulator driving a real parser and model.
struct Screen {
    Screen()
        : harness({}, true)
        , model(40, 10)
        , parser(model) {
        harness.link->setEvents(
            {[this](std::string_view bytes) {
                 raw.append(bytes);
                 parser.feed(bytes);
             },
             [this](int code, int) { exits.push_back(code); },
             nullptr});
        harness.at(0);
    }
    // Type bytes and let the simulator answer.
    void type(std::string_view bytes) {
        EXPECT(harness.link->sendInput(bytes));
        harness.at(0);
    }
    std::string row(std::size_t y) const {
        std::string text;
        for (std::size_t x = 0; x < model.columns(); ++x) {
            const auto cp = model.cell(x, y).codePoint;
            text.push_back(cp >= 32 && cp < 127 ? char(cp) : ' ');
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }

    Harness harness;
    TerminalModel model;
    VtParser parser;
    std::string raw;
    std::vector<int> exits;
};
// Emits "." every 100 ms from the time of its first tick.
class Ticker final : public ScriptedLink::Program {
public:
    ScriptedLink::Reaction start() override {
        return {"start;", std::nullopt};
    }
    ScriptedLink::Reaction input(std::string_view bytes) override {
        return {std::string(bytes), std::nullopt};
    }
    void resize(int, int) override {}
    ScriptedLink::Reaction tick(protocol::Time now) override {
        if (!next_) {
            next_ = now + milliseconds(100);
        }
        ScriptedLink::Reaction reaction;
        while (now >= *next_) {
            reaction.output += ".";
            *next_ += milliseconds(100);
        }
        return reaction;
    }
    std::optional<protocol::Time> nextDeadline() const override {
        return next_;
    }

private:
    std::optional<protocol::Time> next_;
};
const std::string kPromptRow = "matterm:~$";
} // namespace

TEST_SUIT_BEGIN(ScriptedLink)

TEST_CASE("Unpaced output is delivered at the first tick, in order") {
    Harness h(Script().output("one ").output("two"));
    EXPECT(h.output.empty()); // Nothing runs outside tick().
    h.at(0);
    EXPECT(h.output == "one two");
    EXPECT(!h.link->nextDeadline());
}

TEST_CASE("Paced output streams at bytesPerSecond") {
    Harness h(Script().output("0123456789", 10).output("!"));
    h.at(0);
    EXPECT(h.take().empty());
    EXPECT(*h.link->nextDeadline() == protocol::Time(100)); // First byte.
    h.at(500);
    EXPECT(h.take() == "01234");
    h.at(999);
    EXPECT(h.take() == "5678");
    h.at(1000);
    EXPECT(h.take() == "9!");
    EXPECT(h.exits.empty());
}

TEST_CASE("Pacing does not depend on how often tick is called") {
    Harness coarse(
        Script().output("abcdef", 20).delay(milliseconds(100)).output("Z"));
    coarse.at(0);
    coarse.at(5000);
    EXPECT(coarse.output == "abcdefZ");
    Harness fine(
        Script().output("abcdef", 20).delay(milliseconds(100)).output("Z"));
    for (long long t = 0; t < 320; t += 7) {
        fine.at(t);
        if (t < 400) {
            EXPECT(fine.output.find('Z') == std::string::npos || t >= 400);
        }
    }
    // 6 bytes at 20/s end at 300 ms, then the delay ends at 400 ms.
    EXPECT(fine.output == "abcdef");
    fine.at(399);
    EXPECT(fine.output == "abcdef");
    fine.at(400);
    EXPECT(fine.output == "abcdefZ");
}

TEST_CASE("Delay holds back later steps and reports its deadline") {
    Harness h(Script().output("a").delay(milliseconds(100)).output("b"));
    h.at(10);
    EXPECT(h.output == "a");
    EXPECT(*h.link->nextDeadline() == protocol::Time(110));
    h.at(109);
    EXPECT(h.output == "a");
    h.at(110);
    EXPECT(h.output == "ab");
}

TEST_CASE("waitForInput with a literal consumes up to the match") {
    Harness h(Script().output("login: ").waitForInput("\r").output("ok\r\n"));
    h.at(0);
    EXPECT(h.take() == "login: ");
    EXPECT(!h.link->nextDeadline());
    EXPECT(h.link->sendInput("bob"));
    h.at(1);
    EXPECT(h.take().empty()); // No newline yet.
    EXPECT(h.link->sendInput("\r"));
    EXPECT(h.link->nextDeadline().has_value());
    h.at(2);
    EXPECT(h.take() == "ok\r\n");
}

TEST_CASE("Typeahead is kept for a later wait") {
    Harness h(Script()
                  .waitForInput("a")
                  .delay(milliseconds(50))
                  .waitForInput("b")
                  .output("done"));
    h.at(0);
    EXPECT(h.link->sendInput("axb"));
    h.at(10);
    EXPECT(h.output.empty());
    h.at(59);
    EXPECT(h.output.empty());
    h.at(60);
    EXPECT(h.output == "done");
}

TEST_CASE("waitForInput with a predicate sees the pending input") {
    std::vector<std::string> seen;
    Harness h(Script()
                  .waitForInput([&](std::string_view input) {
                      seen.emplace_back(input);
                      return input.size() >= 3;
                  })
                  .output("go"));
    h.link->sendInput("ab");
    h.at(0);
    EXPECT(h.output.empty());
    h.link->sendInput("c");
    h.at(1);
    EXPECT(h.output == "go");
    EXPECT(seen.size() == 2);
    EXPECT(seen[1] == "abc");
}

TEST_CASE("Exit is reported once and refuses further input") {
    Harness h(Script().output("bye").exit(7).output("never"));
    h.at(0);
    EXPECT(h.output == "bye");
    EXPECT(h.exits == std::vector<int>{7});
    EXPECT(h.link->state() == ScriptedLink::State::Exited);
    EXPECT(!h.link->sendInput("x"));
    h.at(10);
    h.link->resize(100, 30);
    h.at(20);
    EXPECT(h.exits.size() == 1);
    EXPECT(h.output == "bye");
    EXPECT(h.closes == 0);
    EXPECT(!h.link->nextDeadline());
}

TEST_CASE("Close ends the session silently") {
    Harness h(Script().output("a").delay(milliseconds(10)).output("b"));
    h.at(0);
    h.link->close();
    h.at(100);
    EXPECT(h.output == "a");
    EXPECT(h.exits.empty());
    EXPECT(!h.link->sendInput("x"));
}

TEST_CASE("Input beyond the typeahead limit is refused") {
    ScriptedLink::Config config;
    config.clock = [] { return protocol::Time(0); };
    config.maxPendingInput = 4;
    ScriptedLink link(config, Script().waitForInput("never"));
    EXPECT(link.sendInput("abcd"));
    EXPECT(!link.sendInput("e"));
}

TEST_CASE("Callbacks that send input do not re-enter the link") {
    Harness h(Script().output("q").waitForInput("r").output("done"));
    h.link->setEvents({[&](std::string_view bytes) {
                           h.output.append(bytes);
                           h.link->sendInput("r");
                           h.link->tick(); // Ignored while ticking.
                       },
                       nullptr,
                       nullptr});
    h.at(0);
    EXPECT(h.output == "qdone");
}

TEST_CASE("Simulator shows a prompt that the parser renders") {
    Screen s;
    EXPECT(s.row(0) == "matterm simulator. Type 'help'.");
    EXPECT(s.row(1) == kPromptRow);
    EXPECT(s.model.cursor().column == 11);
    EXPECT(s.model.cursor().row == 1);
}

TEST_CASE("Typed text is echoed and Backspace erases it") {
    Screen s;
    s.type("helx");
    EXPECT(s.row(1) == kPromptRow + " helx");
    s.type("\x7f");
    s.type("p");
    EXPECT(s.row(1) == kPromptRow + " help");
    s.type("\b\b\b\b\b\b"); // More than typed: stops at the prompt.
    EXPECT(s.row(1) == kPromptRow);
    EXPECT(s.model.cursor().column == 11);
}

TEST_CASE(
    "Enter runs the line, also as CR LF, and a blank line gives a prompt") {
    Screen s;
    s.type("echo a\r\n");
    EXPECT(s.row(2) == "a");
    EXPECT(s.row(3) == kPromptRow);
    s.type("\r");
    EXPECT(s.row(4) == kPromptRow);
    EXPECT(s.model.cursor().row == 4);
}

TEST_CASE("echo prints its arguments with single spacing") {
    Screen s;
    s.type("echo   hello   world  \r");
    EXPECT(s.row(2) == "hello   world");
    s.type("echo\r");
    EXPECT(s.row(3) == kPromptRow + " echo");
    EXPECT(s.row(4).empty());
    EXPECT(s.row(5) == kPromptRow);
}

TEST_CASE("help lists the commands") {
    Screen s;
    s.type("help\r");
    std::string all;
    for (std::size_t y = 0; y < 10; ++y) {
        all += s.row(y) + "\n";
    }
    for (const char *name :
         {"help", "echo", "clear", "colors", "size", "exit"}) {
        EXPECT(all.find(std::string("  ") + name) != std::string::npos);
    }
}

TEST_CASE("clear empties the screen and puts the prompt at the top") {
    Screen s;
    s.type("echo x\r");
    s.type("clear\r");
    EXPECT(s.row(0) == kPromptRow);
    EXPECT(s.row(1).empty());
    EXPECT(s.model.cursor().row == 0);
    EXPECT(s.raw.find("\x1b[H\x1b[2J") != std::string::npos);
}

TEST_CASE("colors uses indexed, truecolor and attribute sequences") {
    Screen s;
    s.type("colors\r");
    EXPECT(s.raw.find("\x1b[41m") != std::string::npos);
    EXPECT(s.raw.find("\x1b[107m") != std::string::npos);
    EXPECT(s.raw.find("\x1b[48;2;") != std::string::npos);
    EXPECT(s.row(5) == "bold faint italic underline inverse");
    EXPECT(s.row(6) == kPromptRow);
    // Row 4 is the truecolor gradient from (0,255,160) to (255,0,160).
    EXPECT(s.model.cell(0, 4).background.g == 255);
    EXPECT(s.model.cell(39, 4).background.r == 255);
}

TEST_CASE("resize reaches the simulator") {
    Screen s;
    s.type("size\r");
    EXPECT(s.row(2) == "40x10");
    s.harness.link->resize(100, 30);
    s.type("size\r");
    EXPECT(s.row(4) == "100x30");
}

TEST_CASE("The line length follows the window width") {
    Screen s;
    s.type(std::string(60, 'x'));
    // 40 columns - 11 prompt - 1 spare.
    EXPECT(s.row(1) == kPromptRow + " " + std::string(28, 'x'));
    EXPECT(s.model.cursor().row == 1);
    s.harness.link->resize(60, 10);
    s.type(std::string(60, 'y'));
    EXPECT(s.raw.find(std::string(20, 'y')) != std::string::npos);
}

TEST_CASE("exit ends the session with the code, once") {
    Screen s;
    s.type("exit 3\r");
    EXPECT(s.exits == std::vector<int>{3});
    EXPECT(!s.harness.link->sendInput("x"));
}

TEST_CASE("exit with a bad argument fails like a shell, plain exit gives 0") {
    Screen s;
    s.type("exit abc\r");
    EXPECT(s.exits == std::vector<int>{2});
    EXPECT(s.raw.find("numeric argument required") != std::string::npos);
    Screen t;
    t.type("exit\r");
    EXPECT(t.exits == std::vector<int>{0});
}

TEST_CASE("Unknown commands are reported and the prompt returns") {
    Screen s;
    s.type("frobnicate now\r");
    EXPECT(s.row(2) == "matterm: frobnicate: command not found");
    EXPECT(s.row(3) == kPromptRow);
}

TEST_CASE("Ctrl-C abandons the line and Ctrl-D on an empty line exits") {
    Screen s;
    s.type("abc\x03");
    EXPECT(s.row(1) == kPromptRow + " abc^C");
    EXPECT(s.row(2) == kPromptRow);
    s.type("\x04");
    EXPECT(s.exits == std::vector<int>{0});
}

TEST_CASE("Escape sequences in the input are ignored") {
    Screen s;
    const auto before = s.raw;
    // Arrow key, cursor position report, paste markers, an OSC reply.
    s.type("\x1b[A\x1b[12;40R\x1bOP\x1b]11;rgb:0/0/0\x1b\\\x1b[200~");
    EXPECT(s.raw == before);
    s.type("ok\x1b[201~");
    EXPECT(s.row(1) == kPromptRow + " ok");
}

TEST_CASE("UTF-8 text survives editing") {
    Screen s;
    s.type("echo \xc3\xa5\xc3\xa4");
    s.type("\x7f"); // Removes one code point, not one byte.
    s.type("\r");
    EXPECT(s.model.cell(0, 2).codePoint == U'å');
    EXPECT(s.model.cell(1, 2).codePoint != U'ä');
}

TEST_CASE("A script can lead into the simulator") {
    ScriptedLink::Config config;
    int now = 0;
    config.clock = [&] { return protocol::Time(now); };
    ScriptedLink link(config,
                      Script().output("boot\r\n"),
                      std::make_unique<Simulator>(80, 24));
    std::string out;
    link.setEvents(
        {[&](std::string_view b) { out.append(b); }, nullptr, nullptr});
    link.tick();
    EXPECT(out.rfind("boot\r\n", 0) == 0);
    EXPECT(out.find("matterm simulator") != std::string::npos);
}

TEST_CASE("A program's tick runs on each link tick and sets the deadline") {
    long long now = 0;
    ScriptedLink::Config config;
    config.clock = [&] { return protocol::Time(now); };
    ScriptedLink link(config, Script(), std::make_unique<Ticker>());
    std::string out;
    link.setEvents(
        {[&](std::string_view b) { out.append(b); }, nullptr, nullptr});
    link.tick();
    EXPECT(out == "start;");
    EXPECT(*link.nextDeadline() == protocol::Time(100));
    now = 99;
    link.tick();
    EXPECT(out == "start;");
    now = 350;
    link.tick();
    EXPECT(out == "start;...");
    EXPECT(*link.nextDeadline() == protocol::Time(400));
    link.sendInput("x");
    EXPECT(*link.nextDeadline() == protocol::Time(350)); // Input is due now.
    link.tick();
    EXPECT(out == "start;...x");
}

TEST_CASE("Programs without tick keep no deadline") {
    Harness h({}, true);
    h.at(0);
    EXPECT(!h.link->nextDeadline());
}

TEST_SUIT_END
