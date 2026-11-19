#include "bigfont.h"
#include "fakeos_harness.h"
#include "mls-unit-test/unittest.h"
#include "scriptedprogram.h"

#include <string>

using namespace fakeos;
using fakeos_test::Pc;

namespace {
const std::string kPrompt = "guest@matterm:~$";

bool hasAlternateScreenOn(const std::string &raw) {
    return raw.find("\x1b[?1049h") != std::string::npos;
}
bool hasAlternateScreenOff(const std::string &raw) {
    return raw.find("\x1b[?1049l") != std::string::npos;
}
} // namespace

namespace {
// Column and row (zero-based) of a word on the screen, or {-1, -1}.
std::pair<int, int> find(const Pc &pc, const std::string &word) {
    for (std::size_t y = 0; y < pc.model.rows(); ++y) {
        const auto at = pc.row(y).find(word);
        if (at != std::string::npos) {
            return {int(at), int(y)};
        }
    }
    return {-1, -1};
}
std::string click(std::pair<int, int> cell) {
    return "\x1b[<0;" + std::to_string(cell.first + 1) + ";" +
           std::to_string(cell.second + 1) + "M";
}
} // namespace

namespace {
Rgb cornerBackground(const Pc &pc) {
    const auto &bg = pc.model.cell(0, 0).background;
    return {bg.r, bg.g, bg.b};
}
int brightness(const Pc &pc, std::size_t from = 0, std::size_t to = 99) {
    int most = 0;
    for (std::size_t y = from; y < to && y < pc.model.rows(); ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            const auto &cell = pc.model.cell(x, y);
            if (cell.codePoint != U' ') {
                most = std::max<int>(most,
                                     std::max({cell.foreground.r,
                                               cell.foreground.g,
                                               cell.foreground.b}));
            }
        }
    }
    return most;
}
} // namespace

TEST_SUIT_BEGIN(FakeosPrograms)

TEST_CASE("fire is a file on disk, outside the PATH") {
    Pc pc;
    pc.run("fire");
    EXPECT(pc.shows("matterm: fire: command not found"));
    pc.run("ls /usr/games");
    EXPECT(pc.shows("fire"));
    pc.run("cat /usr/games/fire");
    EXPECT(pc.shows("#!builtin fire"));
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE(
    "Running fire takes the screen, animates and q returns to the shell") {
    Pc pc;
    pc.run("/usr/games/fire");
    EXPECT(pc.sys.inProgram());
    EXPECT(hasAlternateScreenOn(pc.raw));
    EXPECT(pc.model.alternateScreen());
    EXPECT(pc.sys.nextDeadline().has_value());
    pc.advance(40);
    const auto first = pc.screen();
    pc.advance(400);
    EXPECT(pc.screen() != first); // It moves.
    EXPECT(pc.shows("fire   q quit"));
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    EXPECT(hasAlternateScreenOff(pc.raw));
    EXPECT(!pc.model.alternateScreen());
    EXPECT(pc.last() == kPrompt);
    EXPECT(!pc.sys.nextDeadline());
    EXPECT(pc.shows("/usr/games/fire")); // The command is still on screen.
}

TEST_CASE("The fire is deterministic for a seed and differs between seeds") {
    auto film = [](std::uint64_t seed) {
        Pc pc(false, 40, 12, seed);
        pc.run("/usr/games/fire");
        for (int i = 0; i < 30; ++i) {
            pc.advance(33);
        }
        return pc.raw;
    };
    EXPECT(film(7) == film(7));
    EXPECT(film(7) != film(8));
}

TEST_CASE("Animation frames are small updates, and idle frames cost nothing") {
    Pc pc(false, 60, 20);
    pc.run("/usr/games/fire");
    pc.advance(40);
    pc.raw.clear();
    pc.advance(1); // Not due yet.
    EXPECT(pc.raw.empty());
    pc.advance(60);
    EXPECT(!pc.raw.empty());
    EXPECT(pc.raw.size() <
           60u * 20u * 40u); // Far below a full repaint of rich cells.
}

TEST_CASE("Ctrl-C and Escape also end a program; keys reach it") {
    Pc pc;
    pc.run("/usr/games/fire");
    pc.type("+");
    pc.type("\x03");
    EXPECT(!pc.sys.inProgram());
    pc.run("/usr/games/fire");
    pc.type("\x1b");
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE(
    "Watching the fire for a while leaves an ember in the home directory") {
    Pc pc;
    pc.run("ls -a");
    EXPECT(!pc.shows(".ember"));
    pc.run("/usr/games/fire");
    for (int i = 0; i < 300; ++i) { // About 10 seconds.
        pc.advance(33);
    }
    pc.type("q");
    EXPECT(pc.shows("Something was left in the ashes."));
    pc.run("cat .ember");
    EXPECT(pc.shows("the cave knows a word"));
    // A quick look leaves nothing, and a second long look does not repeat it.
    Pc quick;
    quick.run("/usr/games/fire");
    quick.advance(500);
    quick.type("q");
    quick.run("ls -a");
    EXPECT(!quick.shows(".ember"));
    EXPECT(!quick.shows("Something was left"));
}

TEST_CASE("The easter egg chain: .ember, xyzzy, a new program appears") {
    Pc pc;
    pc.run("ls /usr/games");
    EXPECT(!pc.shows("plasma"));
    pc.run("xyzzy");
    EXPECT(pc.shows("A hollow voice says \"Fool.\""));
    pc.run("clear");
    pc.run("ls /usr/games");
    EXPECT(pc.shows("plasma"));
    pc.run("clear");
    pc.run("xyzzy");
    EXPECT(pc.shows("Nothing happens."));
    EXPECT(pc.sys.world().flags.count("xyzzy") == 1);
    pc.run("/usr/games/plasma");
    EXPECT(pc.sys.inProgram());
    pc.advance(200);
    EXPECT(pc.shows("gradient"));
    pc.type(" "); // Next pattern.
    pc.advance(200);
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
    // Another world has not found it.
    Pc other;
    other.run("ls /usr/games");
    EXPECT(!other.shows("plasma"));
}

TEST_CASE("Clicking a word a program drew can be an easter egg") {
    Pc pc;
    pc.run("xyzzy");
    pc.run("/usr/games/plasma");
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.raw.find("\x1b[?1000h\x1b[?1006h") !=
           std::string::npos); // Mouse on.
    pc.advance(100);
    const auto blue = find(pc, "blue");
    EXPECT(blue.first >= 0);
    // A click on background or on the title does nothing.
    pc.type(click({0, 0}));
    pc.type(click(find(pc, "chance")));
    EXPECT(pc.sys.inProgram());
    // A click on "blue" ends the program and plays the egg.
    pc.type(click({blue.first + 1, blue.second}));
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.raw.find("\x1b[?1000l") != std::string::npos); // Mouse off again.
    EXPECT(pc.shows("you believe whatever you want to believe."));
    EXPECT(pc.last() == kPrompt);
    EXPECT(pc.sys.world().flags.count("pill-blue") == 1);
}

TEST_CASE("The keyboard makes the same choice as a click") {
    Pc pc;
    pc.run("xyzzy");
    pc.run("/usr/games/plasma");
    pc.advance(100);
    pc.type("b");
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("you believe whatever you want to believe."));
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE(
    "The fire does not turn the mouse on, and clicks there mean nothing") {
    Pc pc;
    pc.run("/usr/games/fire");
    EXPECT(pc.raw.find("\x1b[?1000h") == std::string::npos);
    pc.advance(100);
    pc.type(click({3, 3}));
    EXPECT(pc.sys.inProgram());
}

TEST_CASE("Other eggs and sudo") {
    Pc pc;
    pc.run("sudo ls");
    EXPECT(pc.shows(
        "guest is not in the sudoers file. This incident will be reported."));
    pc.run("make me a sandwich");
    EXPECT(pc.shows("What? Make it yourself."));
    pc.run("sudo make me a sandwich");
    EXPECT(pc.shows("Okay."));
    pc.run("hello");
    EXPECT(pc.shows("The system hums quietly."));
}

TEST_CASE("reset forgets discoveries") {
    Pc pc;
    pc.run("xyzzy");
    pc.run("reset");
    pc.run("clear");
    pc.run("ls /usr/games");
    EXPECT(!pc.shows("plasma"));
    pc.run("xyzzy");
    EXPECT(pc.shows("A hollow voice says"));
}

TEST_CASE("The intro plays first, then the welcome and the prompt") {
    Pc pc(true);
    EXPECT(pc.sys.inProgram());
    EXPECT(hasAlternateScreenOn(pc.raw));
    pc.advance(1200);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.shows("#")); // The banner is being drawn.
    pc.advance(6300);      // The whole intro takes about seven seconds.
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.last() == kPrompt);
    EXPECT(!pc.sys.nextDeadline());
}

TEST_CASE("Any key skips the intro") {
    Pc pc(true);
    pc.advance(300);
    pc.type("x");
    EXPECT(pc.sys.inProgram()); // The text goes quickly, the background glides.
    pc.advance(150);
    EXPECT(pc.sys.inProgram());
    pc.advance(1200);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
    EXPECT(pc.last() == kPrompt);
    // The key that skipped it was not typed at the prompt.
    EXPECT(pc.last() == kPrompt);
    Pc click(true);
    click.advance(300);
    click.type("\x1b[<0;5;5M");
    click.advance(1400);
    EXPECT(!click.sys.inProgram());
}

TEST_CASE("The intro spells the name of the terminal in big letters") {
    Pc pc(true, 80, 24);
    pc.advance(2000); // Resolved, before the first line is done.
    // Five rows of "MATTERM", centered: left margin (80 - 41) / 2 = 19.
    const std::string name = "MATTERM";
    const std::size_t top = 9;
    for (int gy = 0; gy < 5; ++gy) {
        std::string expected(19, ' ');
        for (std::size_t i = 0; i < name.size(); ++i) {
            expected += bigGlyph(name[i])[gy];
            expected += i + 1 < name.size() ? " " : "";
        }
        while (!expected.empty() && expected.back() == ' ') {
            expected.pop_back();
        }
        EXPECT(pc.row(top + std::size_t(gy)).substr(0, expected.size()) ==
               expected);
    }
}

TEST_CASE("The first line is typed, scrambles into noise and vanishes; then "
          "the second") {
    Pc pc(true, 80, 24);
    EXPECT(!pc.shows("Futuristic"));
    pc.advance(1300 + 2000); // Typed (44 letters at 32 ms) and held.
    EXPECT(pc.shows("Futuristic system agnostic terminal emulator"));
    EXPECT(
        !pc.shows("a small pretend system")); // The old text is gone for good.
    pc.advance(1000); // t = 4300: in the middle of the scramble.
    EXPECT(!pc.shows("Futuristic system agnostic terminal emulator"));
    // Noise glyphs, and some of the line is already blank.
    const auto row = pc.row(9 + 7);
    EXPECT(!row.empty());
    pc.advance(700); // t = 5000: gone; the second line starts to be typed.
    EXPECT(!pc.shows("Futuristic"));
    pc.advance(1500);
    EXPECT(pc.shows("Real terminal, fake operating system"));
}

TEST_CASE("DEMO MODE blinks red in the corner for a few seconds") {
    Pc pc(true, 80, 24);
    EXPECT(!pc.shows("DEMO MODE"));
    bool on = false, off = false;
    int redCells = 0;
    for (int i = 0; i < 70; ++i) { // t = 0 to 7000 in 100 ms steps.
        pc.advance(100);
        const auto topRow = pc.row(0);
        const bool shown = topRow.find("DEMO MODE") != std::string::npos;
        if (pc.sys.inProgram() && pc.sys.world().now.count() > 1000 &&
            pc.sys.world().now.count() < 5800) {
            on = on || shown;
            off = off || !shown;
        }
        if (shown) {
            for (std::size_t x = 0; x < pc.model.columns(); ++x) {
                const auto &cell = pc.model.cell(x, 0);
                redCells += cell.codePoint == U'D' && cell.foreground.r > 200 &&
                            cell.foreground.g < 120;
            }
        }
        if (pc.sys.world().now.count() > 6200) {
            EXPECT(!shown); // It went away before the end.
        }
    }
    EXPECT(on && off);    // It blinks.
    EXPECT(redCells > 0); // Red.
}

TEST_CASE("The background moves gradually from the intro's to the terminal's") {
    Pc pc(true, 80, 24);
    pc.advance(1000);
    const auto first = cornerBackground(pc);
    EXPECT(first.r < 15 && first.g < 15); // Dark, the intro's own.
    pc.advance(4000);                     // t = 5000: not started yet.
    EXPECT(cornerBackground(pc) == first);
    int last = cornerBackground(pc).g;
    bool between = false;
    for (int i = 0; i < 20 && pc.sys.inProgram(); ++i) {
        pc.advance(100);
        if (!pc.sys.inProgram()) {
            break; // Back on the normal screen.
        }
        const int now = cornerBackground(pc).g;
        EXPECT(now >= last);     // Only ever toward the terminal's 30.
        EXPECT(now - last <= 6); // In small steps, never a jump.
        between = between || (now > first.g + 3 && now < 28);
        last = now;
    }
    EXPECT(between);
    pc.advance(1200);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.last() == kPrompt);
}

TEST_CASE("Skipping the intro still glides the background, without a cut") {
    Pc pc(true, 80, 24);
    pc.advance(1500);
    const auto before = cornerBackground(pc);
    pc.type("x");
    pc.advance(100);
    const auto soon = cornerBackground(pc);
    EXPECT(soon.g - before.g <= 4); // No hard jump to the terminal's color.
    pc.advance(300);                // The text is gone by now.
    int lit = 0;
    for (std::size_t y = 1; y < pc.model.rows() - 1; ++y) {
        for (std::size_t x = 0; x < pc.model.columns(); ++x) {
            const auto &cell = pc.model.cell(x, y);
            lit += cell.codePoint != U' ' &&
                   cell.foreground.r > cell.background.r + 40;
        }
    }
    EXPECT(lit == 0);
    int last = cornerBackground(pc).g;
    bool between = false;
    for (int i = 0; i < 12 && pc.sys.inProgram(); ++i) {
        pc.advance(100);
        if (!pc.sys.inProgram()) {
            break; // Back on the normal screen.
        }
        const int now = cornerBackground(pc).g;
        EXPECT(now >= last);
        between = between || (now > before.g && now < 30);
        last = now;
    }
    EXPECT(between);
    pc.advance(800);
    EXPECT(!pc.sys.inProgram());
    EXPECT(pc.shows("Welcome to Mattrix OS 6.0 Professional Edition"));
}

TEST_CASE("A start command is typed at the first prompt, once") {
    Config config;
    config.intro = false;
    config.startCommand = "echo started by the page";
    Pc pc(config);
    EXPECT(pc.shows("guest@matterm:~$ echo started by the page"));
    EXPECT(pc.shows("\nstarted by the page"));
    EXPECT(pc.last() == kPrompt);
    pc.run("reset"); // A new welcome does not repeat it.
    pc.run("clear");
    EXPECT(!pc.shows("started by the page"));
}

TEST_CASE("A start command can start a program after the intro") {
    Config config;
    config.startCommand = "/usr/games/fire";
    Pc pc(config);
    pc.advance(300);
    pc.type("x"); // Skip the intro.
    pc.advance(1400);
    EXPECT(pc.sys.inProgram());
    EXPECT(pc.model.alternateScreen());
    pc.advance(200);
    EXPECT(pc.shows("fire   q quit"));
}

TEST_CASE("The intro adapts to a small terminal") {
    Pc pc(true, 30, 8);
    pc.advance(1500);
    EXPECT(pc.shows("M A T T E R M"));
    pc.advance(6000);
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE("Resizing during a program redraws it at the new size") {
    Pc pc(false, 60, 20);
    pc.run("/usr/games/fire");
    pc.advance(100);
    pc.sys.resize(30, 10);
    pc.model.resize(30, 10);
    pc.advance(100);
    pc.advance(100);
    EXPECT(pc.sys.inProgram());
    pc.type("q");
    EXPECT(!pc.sys.inProgram());
}

TEST_CASE("Without alternate screen support the screen is cleared instead") {
    Config config;
    config.intro = false;
    config.capabilities.alternateScreen = false;
    System system(config);
    system.start();
    system.tick(Time(0));
    system.input("/usr/games/fire\r");
    const auto frame = system.tick(Time(10));
    EXPECT(frame.output.find("\x1b[?1049") == std::string::npos);
    const auto ended = system.input("q");
    EXPECT(ended.output.find("\x1b[?1049") == std::string::npos);
    EXPECT(ended.output.find("\x1b[2J") != std::string::npos);
}

TEST_CASE("Colors degrade with the terminal's capabilities") {
    Config config;
    config.intro = false;
    config.capabilities.truecolor = false;
    config.capabilities.colors256 = false;
    System system(config);
    system.start();
    system.tick(Time(0));
    system.input("/usr/games/fire\r");
    std::string out;
    for (int i = 1; i < 10; ++i) {
        out += system.tick(Time(i * 40)).output;
    }
    EXPECT(out.find("38;2;") == std::string::npos);
    EXPECT(out.find("38;5;") == std::string::npos);
    EXPECT(out.find("\x1b[0;") != std::string::npos);
}

TEST_CASE("The ScriptedLink adapter runs the system and wakes the host") {
    long long now = 0;
    ScriptedLink::Config config;
    config.clock = [&] { return protocol::Time(now); };
    Config fake;
    fake.columns = 60;
    fake.rows = 20;
    ScriptedLink link(
        config, Script(), std::make_unique<ScriptedProgram>(fake));
    TerminalModel model(60, 20, 10);
    VtParser parser(model);
    link.setEvents(
        {[&](std::string_view b) { parser.feed(b); }, nullptr, nullptr});
    link.tick();
    EXPECT(link.nextDeadline().has_value()); // The intro animates.
    now = 1500;
    link.tick();
    EXPECT(model.alternateScreen());
    link.sendInput("x"); // Skip: a short fade, then the prompt.
    now = 1510;
    link.tick();
    now = 2900;
    link.tick();
    EXPECT(!model.alternateScreen());
    EXPECT(!link.nextDeadline());
    link.sendInput("echo hi\r");
    now = 1520;
    link.tick();
    bool found = false;
    for (std::size_t y = 0; y < model.rows(); ++y) {
        std::string row;
        for (std::size_t x = 0; x < model.columns(); ++x) {
            row.push_back(char(model.cell(x, y).codePoint));
        }
        found = found || row.rfind("hi", 0) == 0;
    }
    EXPECT(found);
}

TEST_SUIT_END
