#include "clioptions.h"
#include "fontpath.h"
#include "render/sdlbackend.h"
#include "terminal/terminalmodel.h"
#include "terminal/vt/vtparser.h"
#ifdef MATTERM_HAS_PTY
#include "ptyfrontend.h"
#endif

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void printUsage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " [font.ttf] [OPTIONS] [-- COMMAND ARG...]\n"
        << "       " << program
        << " [font.ttf] [--font-size PIXELS] [--input FILE | --showcase]\n\n"
        << "Start an interactive shell ($SHELL -i, otherwise /bin/sh -i) by "
           "default.\n"
           "Options must precede --; arguments after -- belong to the "
           "child.\n\n"
           "Getting started: the terminals live in matterm-daemon, which starts by "
           "itself\n"
           "(--no-daemon runs them in the window). To use one from a browser: share it "
           "(the\n"
           "Share button in the tab strip, or Ctrl+Shift+P, \"Share this "
           "terminal\"), run\n"
           "matterm-server --expose-local-sessions as the same user, and in the web "
           "page choose\n"
           "\"Local terminals...\" in the palette (Ctrl+B :). Ctrl+Shift+H lists all "
           "shortcuts;\n"
           "\"Attach terminal...\" in the palette shows a daemon terminal in another "
           "window.\n\n"
           "  --help, -h           Show this help without opening a window.\n"
           "  --font-size PIXELS   Font size, 1..128 renderer pixels (default: "
           "18).\n"
           "  --term TERM          Child TERM (default: xterm-256color).\n"
           "  --daemon             Keep the terminals in matterm-daemon (started if "
           "missing),\n"
           "                       so they survive closing this window (default;\n"
           "                       not with a command; falls back if it cannot start).\n"
           "  --no-daemon          Run the programs in this process.\n"
           "  --daemon-socket PATH Socket of the daemon (default: "
           "$XDG_RUNTIME_DIR/matterm/daemon.sock).\n"
           "  --daemon-host [USER@]HOST\n"
           "                       Use the daemon of another computer through an ssh tunnel\n"
           "                       (started there if missing; needs ssh keys, no questions are\n"
           "                       asked; see docs/REMOTE_DAEMON.md).\n"
           "  --daemon-install     With --daemon-host: use our own matterm-daemon there, copied to\n"
           "                       ~/.local/share/matterm/bin/ when missing (asks first; without\n"
           "                       this it is copied only if the other computer has none on PATH).\n"
           "  --yes                Copy without asking.\n"
           "  --ssh-arg ARG        An extra argument for ssh (repeatable), e.g. -p / 2222.\n"
           "  --completion SHELL   Print the shell completion script (bash, zsh or fish) for "
           "matterm,\n"
           "                       matterm-server and matterm-daemon; see the README.\n"
           "  --no-tab-pulse       Tabs with recent output do not alternate their colour.\n"
           "  --snap-scroll        Let scrolling come to rest on whole rows (default: "
           "smooth\n"
           "                       pixel scrolling, which may stop inside a row).\n"
           "  --smooth-cursor      Animate cursor movement (native PTY only; "
           "default: off).\n"
           "  --record-output FILE Record raw PTY output to a new file (max 64 "
           "MiB).\n"
           "  --input FILE         Play a regular UTF-8/VT file, without a "
           "shell.\n"
           "  --showcase           Show the built-in VT example, without a "
           "shell.\n"
           "  --smoke-test         Run the hidden VT rendering self-test.\n"
           "  --pty-smoke-test     Run the hidden PTY self-test; requires -- "
           "COMMAND.\n"
           "  --pty-tabs-smoke-test Run the hidden tabs and panes self-test; "
           "requires -- COMMAND.\n\n"
           "History navigation is already smooth; there is no --smooth-scroll "
           "flag.\n"
           "Native: wheel / Shift+PageUp/PageDown scroll history; Ctrl+Shift+Q "
           "quits.\n"
           "Tabs and panes: Ctrl+Shift+T/W new/close tab, Ctrl+PgUp/PgDn or "
           "Alt+1..9 switch, Ctrl+Shift+E/O split, Alt+Shift+arrows focus, "
           "Alt+Shift+W close pane, Alt+Shift+E even out; drag a divider "
           "(Shift snaps to cells).\n"
           "Live program output still scrolls by whole rows, not an "
           "animation.\n\n"
        << "Example: " << program << " --smooth-cursor -- /bin/bash -i\n";
#ifndef MATTERM_HAS_PTY
    out << "\nThis build has no native PTY support; the default is the VT "
           "showcase.\n";
#endif
}

std::string startup() {
    std::string bytes = "\x1b[2J\x1b[H\x1b]2;VT stream frontend - PTY pending\a"
                        "\x1b[38;2;120;220;170mmatterm - VT stream frontend\r\n"
                        "\x1b[38;2;160;180;250m";
    for (const std::string_view line :
         {R"(                 _   _                      )",
          R"( _ __ ___   __ _| |_| |_ ___ _ __ _ __ ___  )",
          R"(| '_ ` _ \ / _` | __| __/ _ \ '__| '_ ` _ \ )",
          R"(| | | | | | (_| | |_| ||  __/ |  | | | | | |)",
          R"(|_| |_| |_|\__,_|\__|\__\___|_|  |_| |_| |_|)"}) {
        bytes += line;
        bytes += "\r\n";
    }
    bytes += "\x1b[0m\r\nUTF-8 + CSI + OSC, parsed as an incremental byte "
             "stream.\r\n"
             "ÅÄÖ åäö  |  no shell/PTY yet.\r\n\r\n";
    for (int x = 0; x < 40; ++x) {
        bytes += "\x1b[48;2;" + std::to_string(40 + x * 4) + ";" +
                 std::to_string(60 + x * 2) + ";" +
                 std::to_string(200 - x * 3) + "m ";
    }
    bytes +=
        "\x1b[0m\r\n\r\nUse --input FILE to render a regular UTF-8/VT file.\r\n"
        "Wheel/PgUp/PgDn: history  Home/End: oldest/live\r\n"
        "Space: pause file playback  Escape: exit\r\n";
    return bytes;
}

std::vector<Uint32> pixels(SDL_Window &window, int cellHeight) {
    const SDL_Rect area{0, 0, 400, cellHeight};
    std::vector<Uint32> result(std::size_t(area.w) * area.h);
    if (SDL_RenderReadPixels(SDL_GetRenderer(&window),
                             &area,
                             SDL_PIXELFORMAT_ARGB8888,
                             result.data(),
                             area.w * int(sizeof(Uint32))) < 0) {
        throw std::runtime_error(SDL_GetError());
    }
    return result;
}

void smoke(SDL_Window &window,
           SdlBackend &backend,
           TerminalModel &model,
           VtParser &parser,
           std::string &reply) {
    model.followTail();
    model.advanceViewport(10);
    auto frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    const auto original = pixels(window, backend.cellHeight());
    if (model.cell(0, 0).codePoint != U'm' ||
        model.cell(0, 0).foreground.g != 220 ||
        std::none_of(original.begin(), original.end(), [](Uint32 pixel) {
            return (pixel & 0xffffff) != 0x0e1116;
        })) {
        throw std::runtime_error("VT smoke: startup was not parsed/rendered");
    }
    backend.present();
    parser.feed("\x1b[?1049h\x1b[32mAlternate screen via VT");
    if (!model.alternateScreen()) {
        throw std::runtime_error("VT smoke: alternate mode failed");
    }
    frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    if (pixels(window, backend.cellHeight()) == original) {
        throw std::runtime_error(
            "VT smoke: alternate screen did not change pixels");
    }
    backend.present();
    parser.feed("\x1b[?1049l\x1b[6n");
    if (model.alternateScreen() || reply.empty()) {
        throw std::runtime_error("VT smoke: restore/reply failed");
    }
    frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    if (pixels(window, backend.cellHeight()) != original) {
        throw std::runtime_error("VT smoke: primary pixels not restored");
    }
    backend.present();
    std::cout << "VT frontend smoke test passed\n";
}

int run(const char *font, int fontSize, const char *inputPath, bool smokeTest) {
    std::ifstream file;
    if (inputPath) {
        // Pipes/devices may block the GUI. The future PTY adapter will have its
        // own nonblocking transport; this initial frontend accepts regular
        // files.
        if (!std::filesystem::is_regular_file(inputPath)) {
            throw std::runtime_error("--input requires a regular file");
        }
        file.open(inputPath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Could not open input file");
        }
    }
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("matterm - VT frontend (no PTY)",
                         SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED,
                         960,
                         640,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                             (smokeTest ? SDL_WINDOW_HIDDEN : 0)),
        SDL_DestroyWindow);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SdlBackend backend(*window, font, fontSize, smokeTest);
    auto width = [&] {
        return std::size_t(std::max(1, backend.columns(true)));
    };
    auto height = [&] { return std::size_t(std::max(1, backend.rows())); };
    TerminalModel model(width(), height(), 1000);
    std::string lastReply;
    VtParser parser(model,
                    {[&](std::string_view bytes) { lastReply.assign(bytes); },
                     [&](std::string_view title) {
                         const auto value = "matterm | " + std::string(title);
                         SDL_SetWindowTitle(window.get(), value.c_str());
                     }},
                    {220, 225, 245},
                    {14, 17, 22});
    parser.feed("\x1b[2J\x1b[H");
    if (!inputPath) {
        const auto bytes = startup();
        // The headless test deliberately splits every UTF-8/escape sequence.
        if (smokeTest) {
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                parser.feed(std::string_view(bytes).substr(i, 1));
            }
        }
        else {
            parser.feed(bytes);
        }
        parser.finish();
    }
    if (smokeTest) {
        smoke(*window, backend, model, parser, lastReply);
        return 0;
    }

    bool paused = false;
    bool finished = !inputPath;
    bool running = true;
    Uint64 lastFrame = SDL_GetTicks64();
    while (running) {
        model.resize(width(), height());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                model.scrollBy(amount * 3);
            }
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_SPACE:
                    if (!event.key.repeat) {
                        paused = !paused;
                    }
                    break;
                case SDLK_PAGEUP:
                case SDLK_UP:
                    model.scrollBy(double(model.rows()));
                    break;
                case SDLK_PAGEDOWN:
                case SDLK_DOWN:
                    model.scrollBy(-double(model.rows()));
                    break;
                case SDLK_HOME:
                    model.scrollToOldest();
                    break;
                case SDLK_END:
                    model.followTail();
                    break;
                default:
                    break;
                }
            }
        }
        if (!running) {
            break;
        }
        if (!paused && !finished) {
            std::array<char, 4096> bytes{};
            file.read(bytes.data(), std::streamsize(bytes.size()));
            parser.feed(
                std::string_view(bytes.data(), std::size_t(file.gcount())));
            if (file.bad() || (file.fail() && !file.eof())) {
                throw std::runtime_error("Input read failed");
            }
            if (file.eof()) {
                parser.finish();
                finished = true;
            }
        }
        const auto now = SDL_GetTicks64();
        model.advanceViewport(double(now - lastFrame) / 1000);
        lastFrame = now;
        auto frame = model.frame(backend.cellHeight());
        frame.textBlinkVisible = (now / 500) % 2 == 0;
        frame.cursor.focused =
            (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0;
        frame.cursor.visible = frame.cursor.visible &&
                               (!frame.cursor.focused || !frame.cursor.blink ||
                                (now / 500) % 2 == 0);
        backend.render(frame);
        backend.present();
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    const char *inputPath = nullptr;
    const char *recordPath = nullptr;
    bool smokeTest = false;
    bool ptySmokeTest = false;
    bool ptyTabsSmokeTest = false;
    bool smoothCursor = false;
    bool useDaemon = true; // Default; --no-daemon turns it off.
    bool daemonExplicit = false;
    bool snapScroll = false;
    bool tabPulse = true;
    std::string daemonSocket;
    std::string daemonHost;
    std::vector<std::string> sshArguments;
    bool daemonInstall = false;
    bool assumeYes = false;
    bool showcase = false;
    std::string term = "xterm-256color";
    std::vector<std::string> command;
    const char *fontOverride = nullptr;
    int fontSize = 18;
    int firstOption = 1;
    if (argc > 1 && std::string_view(argv[1]) != "-h" &&
        std::string_view(argv[1]).substr(0, 2) != "--") {
        fontOverride = argv[1];
        firstOption = 2;
    }
    bool valid = true;
    for (int i = firstOption; i < argc && valid; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--") {
            while (++i < argc) {
                command.emplace_back(argv[i]);
            }
            valid = !command.empty();
            break;
        }
        else if (arg == "--help" || arg == "-h") {
            printUsage(std::cout, argv[0]);
            return 0;
        }
        else if (arg == "--font-size") {
            const std::string_view value = ++i < argc ? argv[i] : "";
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, fontSize);
            if (parsed.ec != std::errc{} || parsed.ptr != end || fontSize < 1 ||
                fontSize > 128) {
                std::cerr
                    << "--font-size requires an integer from 1 to 128 pixels\n";
                return 1;
            }
        }
        else if (arg == "--daemon") {
            useDaemon = true;
            daemonExplicit = true;
        }
        else if (arg == "--completion") {
            // The script for matterm, matterm-server and matterm-daemon (docs/README: install).
            const std::string script =
                frontend::cli::completionScript(i + 1 < argc ? std::string(argv[i + 1]) : std::string());
            if (script.empty()) {
                std::cerr << "--completion needs bash, zsh or fish\n";
                return 1;
            }
            std::cout << script;
            return 0;
        }
        else if (arg == "--snap-scroll") {
            snapScroll = true;
        }
        else if (arg == "--no-tab-pulse") {
            tabPulse = false;
        }
        else if (arg == "--no-daemon") {
            useDaemon = false;
        }
        else if (arg == "--daemon-socket" && i + 1 < argc) {
            daemonSocket = argv[++i];
        }
        else if (arg == "--daemon-host" && i + 1 < argc) {
            daemonHost = argv[++i];
            useDaemon = true;
            daemonExplicit = true;
        }
        else if (arg == "--daemon-install") {
            daemonInstall = true;
        }
        else if (arg == "--yes") {
            assumeYes = true;
        }
        else if (arg == "--ssh-arg" && i + 1 < argc) {
            sshArguments.emplace_back(argv[++i]);
        }
        else if (arg == "--smooth-cursor") {
            smoothCursor = true;
        }
        else if (arg == "--showcase") {
            showcase = true;
        }
        else if (arg == "--pty-smoke-test") {
            ptySmokeTest = true;
        }
        else if (arg == "--pty-tabs-smoke-test") {
            ptyTabsSmokeTest = true;
        }
        else if (arg == "--term" && i + 1 < argc) {
            term = argv[++i];
        }
        else if (arg == "--smoke-test") {
            smokeTest = true;
        }
        else if (arg == "--record-output" && i + 1 < argc && !recordPath &&
                 std::string_view(argv[i + 1]).substr(0, 2) != "--") {
            recordPath = argv[++i];
        }
        else if (arg == "--input" && i + 1 < argc) {
            inputPath = argv[++i];
        }
        else {
            valid = false;
        }
    }
    const bool daemonConflict = !command.empty() || inputPath || smokeTest || showcase ||
                                ptySmokeTest || ptyTabsSmokeTest;
    if (useDaemon && !daemonExplicit && daemonConflict) {
        useDaemon = false; // A command, input file or test: run it here.
    }
    if (useDaemon && (!command.empty() || inputPath || smokeTest || showcase || ptySmokeTest ||
                      ptyTabsSmokeTest)) {
        std::cerr << "--daemon runs the daemon's shell ($SHELL); it cannot be combined with a "
                     "command, --input, --showcase or the smoke tests\n";
        return 1;
    }
    if ((daemonInstall || assumeYes) && daemonHost.empty()) {
        std::cerr << "--daemon-install and --yes are for --daemon-host\n";
        return 1;
    }
    if (!daemonHost.empty() && (!daemonSocket.empty() || !useDaemon)) {
        std::cerr << "--daemon-host is its own daemon: it cannot be combined with --daemon-socket or --no-daemon\n";
        return 1;
    }
    if (smoothCursor && (inputPath || smokeTest || showcase)) {
        std::cerr << "--smooth-cursor requires native PTY mode; do not combine "
                     "it with --input, --showcase or --smoke-test\n";
        return 1;
    }
#ifndef MATTERM_HAS_PTY
    if (smoothCursor) {
        std::cerr
            << "--smooth-cursor requires a build with native PTY support\n";
        return 1;
    }
#endif
    if (!valid || (inputPath && smokeTest) ||
        (recordPath && (inputPath || smokeTest || showcase || ptySmokeTest ||
                        ptyTabsSmokeTest)) ||
        ((!command.empty() || ptySmokeTest || ptyTabsSmokeTest) &&
         (inputPath || smokeTest || showcase)) ||
        (ptySmokeTest && ptyTabsSmokeTest) ||
        ((ptySmokeTest || ptyTabsSmokeTest) && command.empty())) {
        printUsage(std::cerr, argv[0]);
        return 1;
    }
    if (!inputPath && !smokeTest && !showcase) {
#ifdef MATTERM_HAS_PTY
        if (command.empty()) {
            const char *shell = std::getenv("SHELL");
            command = {shell && *shell ? shell : "/bin/sh", "-i"};
        }
        try {
            const auto font = applicationFontPath(fontOverride);
            return runPtyFrontend(font.c_str(),
                                  fontSize,
                                  command,
                                  term,
                                  ptySmokeTest,
                                  recordPath,
                                  smoothCursor,
                                  ptyTabsSmokeTest,
                                  DaemonOptions{useDaemon, daemonSocket, daemonExplicit, snapScroll, daemonHost, sshArguments, daemonInstall, assumeYes, tabPulse});
        }
        catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
#else
        if (!command.empty() || ptySmokeTest || recordPath) {
            std::cerr << "This build has no native PTY support\n";
            return 1;
        }
        std::cerr << "PTY disabled: showing the VT showcase instead\n";
#endif
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cerr << TTF_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    int result = 1;
    try {
        const auto font = applicationFontPath(fontOverride);
        result = run(font.c_str(), fontSize, inputPath, smokeTest);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    TTF_Quit();
    SDL_Quit();
    return result;
}
