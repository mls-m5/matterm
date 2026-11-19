#include "mls-unit-test/unittest.h"
#include "ptylink.h"

#include <chrono>
#include <string>
#include <thread>

// matterm-demo --tty under a real pseudo-terminal: raw mode, the pretend shell,
// a program on the alternate screen, capability detection, resize and exit.
namespace {
struct Session {
    Session(const std::string &term,
            PtyProcess::ColorCapability color,
            bool intro = false)
        : link(intro ? std::vector<std::string>{DEMO_BINARY,
                                                "--tty",
                                                "--seed",
                                                "3"}
                     : std::vector<std::string>{DEMO_BINARY,
                                                "--tty",
                                                "--no-intro",
                                                "--seed",
                                                "3"},
               80,
               24,
               term,
               color) {
        link.setEvents(
            {[this](std::string_view bytes) { output.append(bytes); },
             [this](int code, int) {
                 exited = true;
                 exitCode = code;
             },
             {}});
    }
    bool until(const std::string &needle, int ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (output.find(needle, from) == std::string::npos &&
               std::chrono::steady_clock::now() < deadline && !exited) {
            link.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const bool found = output.find(needle, from) != std::string::npos;
        if (found) {
            from = output.find(needle, from) + needle.size();
        }
        return found;
    }
    bool untilExit(int ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!exited && std::chrono::steady_clock::now() < deadline) {
            link.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return exited;
    }
    PtyLink link;
    std::string output;
    std::size_t from = 0;
    bool exited = false;
    int exitCode = -2;
};
} // namespace

TEST_SUIT_BEGIN(DemoTty)

TEST_CASE("The shell works in a real terminal and exit restores it") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor);
    EXPECT(s.until("guest@matterm"));
    EXPECT(s.output.find("Welcome to matterm 1.0") != std::string::npos);
    s.link.sendInput("echo tty works\r");
    EXPECT(s.until("tty works"));
    s.link.sendInput("exit 4\r");
    EXPECT(s.untilExit());
    EXPECT_EQ(s.exitCode, 4);
    EXPECT(s.output.find("\x1b[?25h") !=
           std::string::npos); // Cursor shown again.
}

TEST_CASE("A program takes the alternate screen and returns on q") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor);
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("/usr/games/fire\r");
    EXPECT(s.until("\x1b[?1049h"));
    EXPECT(s.until("fire   q quit"));
    EXPECT(s.output.find("38;2;") !=
           std::string::npos); // Truecolor on request.
    s.link.sendInput("q");
    EXPECT(s.until("\x1b[?1049l"));
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("\x04"); // Ctrl-D: logout.
    EXPECT(s.untilExit());
    EXPECT_EQ(s.exitCode, 0);
}

TEST_CASE("Without truecolor the colors are reduced to 256") {
    Session s("xterm-256color", PtyProcess::ColorCapability::Unspecified);
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("/usr/games/fire\r");
    EXPECT(s.until("fire   q quit"));
    EXPECT(s.output.find("38;2;") == std::string::npos);
    EXPECT(s.output.find("38;5;") != std::string::npos);
    s.link.sendInput("q");
    EXPECT(s.until("\x1b[?1049l"));
}

TEST_CASE("On a plain console there is no alternate screen") {
    Session s("linux", PtyProcess::ColorCapability::Unspecified);
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("/usr/games/fire\r");
    EXPECT(s.until("fire   q quit"));
    EXPECT(s.output.find("\x1b[?1049") == std::string::npos);
    EXPECT(s.output.find("38;2;") == std::string::npos);
    EXPECT(s.output.find("38;5;") == std::string::npos); // 16 colors only.
    s.link.sendInput("q");
    EXPECT(s.until("guest@matterm"));
}

TEST_CASE("The intro plays and any key skips it") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor, true);
    EXPECT(s.until("\x1b[?1049h"));
    EXPECT(s.until("#")); // The banner.
    s.link.sendInput(" ");
    EXPECT(s.until("\x1b[?1049l"));
    EXPECT(s.until("Welcome to matterm 1.0"));
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("exit\r");
    EXPECT(s.untilExit());
}

TEST_CASE("A resize reaches a running program") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor);
    EXPECT(s.until("guest@matterm"));
    s.link.sendInput("/usr/games/fire\r");
    EXPECT(s.until("fire   q quit"));
    s.link.resize(100, 30);
    EXPECT(s.until("\x1b[30;")); // Cursor addressing now reaches row 30.
    s.link.sendInput("q");
    EXPECT(s.until("guest@matterm"));
}

TEST_SUIT_END
