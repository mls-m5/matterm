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

int run(const char *font, const char *inputPath, bool smokeTest) {
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
    SdlBackend backend(*window, font, 24, smokeTest);
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
        frame.cursor.visible = frame.cursor.visible && (now / 500) % 2 == 0;
        backend.render(frame);
        backend.present();
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    const char *inputPath = nullptr;
    bool smokeTest = false;
    bool ptySmokeTest = false;
    bool showcase = false;
    std::string term = "vt100";
    std::vector<std::string> command;
    bool valid = argc >= 2;
    for (int i = 2; i < argc && valid; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--") {
            while (++i < argc) {
                command.emplace_back(argv[i]);
            }
            valid = !command.empty();
            break;
        }
        else if (arg == "--showcase") {
            showcase = true;
        }
        else if (arg == "--pty-smoke-test") {
            ptySmokeTest = true;
        }
        else if (arg == "--term" && i + 1 < argc) {
            term = argv[++i];
        }
        else if (arg == "--smoke-test") {
            smokeTest = true;
        }
        else if (arg == "--input" && i + 1 < argc) {
            inputPath = argv[++i];
        }
        else {
            valid = false;
        }
    }
    if (!valid || (inputPath && smokeTest) ||
        ((!command.empty() || ptySmokeTest) &&
         (inputPath || smokeTest || showcase)) ||
        (ptySmokeTest && command.empty())) {
        std::cerr << "Usage: " << argv[0]
                  << " /path/to/font.ttf [--term TERM] [-- COMMAND ARG...]\n"
                  << "       " << argv[0]
                  << " /path/to/font.ttf [--input FILE | --showcase | "
                     "--smoke-test]\n";
        return 1;
    }
    if (!inputPath && !smokeTest && !showcase) {
#ifdef MATTERM_HAS_PTY
        if (command.empty()) {
            const char *shell = std::getenv("SHELL");
            command = {shell && *shell ? shell : "/bin/sh", "-i"};
        }
        try {
            return runPtyFrontend(argv[1], command, term, ptySmokeTest);
        }
        catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
#else
        if (!command.empty() || ptySmokeTest) {
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
        result = run(argv[1], inputPath, smokeTest);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    TTF_Quit();
    SDL_Quit();
    return result;
}
