#include "mls-unit-test/unittest.h"
#include "ptylink.h"

#include <chrono>
#include <string>
#include <thread>

// mattrix (the standalone Mattrix OS) under a real pseudo-terminal, the way it
// runs in any terminal and inside matterm: TERM/COLORTERM as matterm sets them.
namespace {
struct Session {
    Session(const std::string &term,
            PtyProcess::ColorCapability color,
            bool intro = false,
            std::vector<std::string> extra = {})
        : link(arguments(intro, std::move(extra)), 80, 24, term, color) {
        link.setEvents(
            {[this](std::string_view bytes) { output.append(bytes); },
             [this](int code, int) {
                 exited = true;
                 exitCode = code;
             },
             {}});
    }
    static std::vector<std::string> arguments(bool intro,
                                              std::vector<std::string> extra) {
        std::vector<std::string> args{MATTRIX_BINARY, "--seed", "3"};
        if (!intro) {
            args.push_back("--no-intro");
        }
        args.insert(args.end(), extra.begin(), extra.end());
        return args;
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
    void pumpFor(int ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline && !exited) {
            link.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
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

TEST_SUIT_BEGIN(MattrixTty)

TEST_CASE("It runs in a terminal under its own name and exit restores it") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor);
    EXPECT(s.until("guest@mattrix"));
    EXPECT(s.output.find("Welcome to Mattrix OS 6.0 Professional Edition") !=
           std::string::npos);
    s.link.sendInput("echo tty works\r");
    EXPECT(s.until("tty works"));
    s.link.sendInput("about\r");
    EXPECT(s.until("runs in your real"));
    s.link.sendInput("exit 3\r");
    EXPECT(s.untilExit());
    EXPECT_EQ(s.exitCode, 3);
    EXPECT(s.output.find("\x1b[?25h") != std::string::npos);
}

TEST_CASE("The intro is Mattrix, in truecolor, and a key skips it") {
    Session s("xterm-256color", PtyProcess::ColorCapability::TrueColor, true);
    EXPECT(s.until("\x1b[?1049h"));
    EXPECT(s.until("#")); // The banner.
    EXPECT(s.output.find("38;2;") != std::string::npos);
    s.link.sendInput(" ");
    EXPECT(s.until("\x1b[?1049l"));
    EXPECT(s.until("guest@mattrix"));
    EXPECT(s.output.find("Futuristic system agnostic") == std::string::npos);
    s.link.sendInput("exit\r");
    EXPECT(s.untilExit());
}

TEST_CASE("256 colors and a plain console are respected") {
    {
        Session s("xterm-256color", PtyProcess::ColorCapability::Unspecified);
        EXPECT(s.until("guest@mattrix"));
        s.link.sendInput("/usr/games/fire\r");
        EXPECT(s.until("fire   q quit"));
        EXPECT(s.output.find("38;2;") == std::string::npos);
        EXPECT(s.output.find("38;5;") != std::string::npos);
        s.link.sendInput("q");
        EXPECT(s.until("guest@mattrix"));
    }
    Session plain("linux", PtyProcess::ColorCapability::Unspecified);
    EXPECT(plain.until("guest@mattrix"));
    plain.link.sendInput("/usr/games/snow\r");
    EXPECT(plain.until("snow   q quit"));
    EXPECT(plain.output.find("\x1b[?1049") == std::string::npos);
    EXPECT(plain.output.find("38;5;") == std::string::npos);
}

TEST_CASE("--help and bad options") {
    PtyLink help({MATTRIX_BINARY, "--help"},
                 80,
                 24,
                 "xterm-256color",
                 PtyProcess::ColorCapability::Unspecified);
    std::string out;
    int code = -2;
    help.setEvents({[&](std::string_view b) { out.append(b); },
                    [&](int c, int) { code = c; },
                    {}});
    for (int i = 0; i < 1000 && code == -2; ++i) {
        help.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(code, 0);
    EXPECT(out.find("Mattrix OS 6.0") != std::string::npos);
    EXPECT(out.find("--no-intro") != std::string::npos);
}

TEST_SUIT_END
