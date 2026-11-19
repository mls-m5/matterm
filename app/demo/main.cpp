// matterm-demo: the pretend system (src/fakeos, docs/FAKEOS.md) in a matterm
// window, or with --tty in the terminal it was started from. Both run the same
// core; the window uses the terminal model and the SDL renderer like the other
// frontends.
#include "beeper.h"
#include "cursorglide.h"
#include "fontpath.h"
#include "inputencoder.h"
#include "palette.h"
#include "scriptedlink.h"
#include "scriptedprogram.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"
#include "vtparser.h"
#include "windowsnap.h"
#ifdef MATTERM_DEMO_TTY
#include "ttyhost.h"
#endif

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace frontend;
using MouseEvent = InputEncoder::MouseEvent;

namespace {
void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " [font.ttf] [OPTIONS]\n\n"
        << "A small simulated Unix with an intro, a shell and programs to "
           "find.\n"
           "Nothing in it touches your real computer.\n\n"
           "  --help, -h           Show this help.\n"
           "  --font-size PIXELS   Font size, 1..128 renderer pixels (default: "
           "18).\n"
           "  --no-intro           Skip the intro animation.\n"
           "  --seed N             Seed for the animations (default: from the "
           "clock).\n"
           "  --no-reveal          Print program output at once, not at a "
           "pace.\n"
           "  --no-smooth-cursor   Jump the cursor between cells (default: it "
           "glides).\n"
           "  --sound              Film-terminal pips for revealed and typed "
           "text.\n"
#ifdef MATTERM_DEMO_TTY
           "  --tty                Run in this terminal instead of a window.\n"
#endif
           "  --smoke-test         Hidden-window self-test, then exit.\n\n"
           "Window: wheel and Shift+PageUp/PageDown scroll, Ctrl+Shift+C/V "
           "copy and\n"
           "paste, Ctrl+Shift+Q quits. `exit` closes the demo.\n";
}

struct SdlRuntime {
    SdlRuntime() {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            throw std::runtime_error(SDL_GetError());
        }
        if (TTF_Init() < 0) {
            const std::string error = TTF_GetError();
            SDL_Quit();
            throw std::runtime_error(error);
        }
    }
    ~SdlRuntime() {
        TTF_Quit();
        SDL_Quit();
    }
};

int runWindow(const char *font,
              int fontSize,
              fakeos::Config pretend,
              bool smokeTest,
              bool smoothCursor,
              bool sound) {
    SdlRuntime runtime;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("matterm demo",
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
    backend.setReserveScrollbarStrip(true);
    pretend.columns = std::clamp(backend.columns(true), 1, 65535);
    pretend.rows = std::clamp(backend.rows(), 1, 65535);

    const Palette defaultPalette;
    Palette palette;
    std::string title = "matterm demo";
    TerminalModel model(
        std::size_t(pretend.columns), std::size_t(pretend.rows), 1000);
    std::unique_ptr<ScriptedLink> link;
    VtEvents vtEvents;
    vtEvents.reply = [&](std::string_view bytes) {
        if (link) {
            link->sendInput(bytes);
        }
    };
    vtEvents.title = [&](std::string_view value) {
        title = "matterm demo | " + std::string(value);
    };
    vtEvents.palette = &palette;
    vtEvents.setPaletteColor = [&](std::size_t index, Rgb8 color) {
        palette.colors[index] = color;
    };
    vtEvents.resetPaletteColor = [&](std::optional<std::size_t> index) {
        if (index) {
            palette.colors[*index] = defaultPalette.colors[*index];
        }
        else {
            palette.colors = defaultPalette.colors;
        }
    };
    VtParser parser(model, std::move(vtEvents));

    bool running = true;
    int exitCode = 0;
    const auto start = std::chrono::steady_clock::now();
    ScriptedLink::Config config;
    config.clock = [start] {
        return protocol::Time(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start));
    };
    config.columns = pretend.columns;
    config.rows = pretend.rows;
    // The pips of revealed and typed text, through SDL audio when asked for.
    std::unique_ptr<Beeper> beeper;
    int pips = 0;
    if (sound) {
        beeper = std::make_unique<Beeper>();
        if (beeper->ok()) {
            pretend.sound = true;
            pretend.onBeep = [&beeper, &pips](const fakeos::Beep &beep) {
                ++pips;
                beeper->play(beep);
            };
        }
        else {
            std::cerr << "No audio device: continuing silently\n";
        }
    }
    link = std::make_unique<ScriptedLink>(
        config, Script(), std::make_unique<fakeos::ScriptedProgram>(pretend));
    TerminalLink::Events events;
    events.output = [&](std::string_view bytes) { parser.feed(bytes); };
    events.exited = [&](int code, int) {
        exitCode = code;
        running = false;
    };
    link->setEvents(std::move(events));

    auto fit = [&] {
        const int columns = std::clamp(backend.columns(true), 1, 65535);
        const int rows = std::clamp(backend.rows(), 1, 65535);
        if (model.columns() != std::size_t(columns) ||
            model.rows() != std::size_t(rows)) {
            model.resize(std::size_t(columns), std::size_t(rows));
            link->resize(columns, rows);
        }
    };
    if (smokeTest) {
        // Intro first frames, a skipped intro (it fades out, about a second), a
        // command, and a drawn frame.
        for (int i = 0; i < 4; ++i) {
            link->tick();
            SDL_Delay(20);
        }
        link->sendInput("x");
        const auto deadline = SDL_GetTicks64() + 4000;
        const auto promptShown = [&] {
            for (std::size_t y = 0; y < model.rows(); ++y) {
                std::string row;
                for (std::size_t x = 0; x < model.columns(); ++x) {
                    row.push_back(char(model.cell(x, y).codePoint));
                }
                if (row.rfind("guest@matterm", 0) == 0) {
                    return true;
                }
            }
            return false;
        };
        while (!promptShown() && SDL_GetTicks64() < deadline) {
            link->tick();
            SDL_Delay(10);
        }
        link->sendInput("echo smoke\r");
        // The output is revealed at a pace: wait for it.
        const auto outputShown = [&] {
            for (std::size_t y = 0; y < model.rows(); ++y) {
                std::string row;
                for (std::size_t x = 0; x < model.columns(); ++x) {
                    row.push_back(char(model.cell(x, y).codePoint));
                }
                if (row.rfind("smoke", 0) == 0) {
                    return true;
                }
            }
            return false;
        };
        const auto outputDeadline = SDL_GetTicks64() + 4000;
        while (!outputShown() && SDL_GetTicks64() < outputDeadline) {
            link->tick();
            SDL_Delay(10);
        }
        const bool found = outputShown();
        auto frame = model.frame(backend.cellHeight());
        frame.palette = &palette;
        backend.render(frame);
        backend.present();
        if (!found) {
            throw std::runtime_error(
                "demo smoke: the command output is missing");
        }
        if (sound && beeper->ok() && pips == 0) {
            throw std::runtime_error(
                "demo smoke: sound is on but no pips were played");
        }
        std::cout << "matterm-demo smoke test passed"
                  << (sound && beeper->ok()
                          ? " (" + std::to_string(pips) + " pips)"
                          : "")
                  << "\n";
        return 0;
    }

    SDL_StartTextInput();
    bool dragging = false;
    std::optional<Selection> selection;
    std::string clipboardError;
    bool mouseCaptured = false;
    MouseEvent::Button heldButton = MouseEvent::Button::None;
    std::optional<std::pair<int, int>> lastMouseCell;
    bool skipText = false;
    const auto modes = [&]() -> const TerminalModes & {
        return parser.modes();
    };
    const auto send = [&](const std::string &bytes) {
        if (!bytes.empty() && link->sendInput(bytes)) {
            model.followTail();
        }
    };
    const auto mouseTracking = [&] {
        return modes().mouseTracking != TerminalModes::MouseTracking::Off;
    };
    const auto sendMouse = [&](MouseEvent::Type type,
                               MouseEvent::Button button,
                               int x,
                               int y,
                               bool clamp,
                               bool dedupe) {
        const auto cell = mouseCell(*window, backend, model, x, y, clamp);
        if (!cell) {
            return;
        }
        const auto first =
            model.firstRetainedRow() + model.viewport().firstRow();
        MouseEvent report;
        report.type = type;
        report.button = button;
        report.modifiers = modifiers(SDL_GetModState());
        report.column = int(cell->column);
        report.row = cell->row >= first ? int(cell->row - first) : 0;
        if (dedupe &&
            lastMouseCell == std::make_pair(report.column, report.row)) {
            return;
        }
        lastMouseCell = std::make_pair(report.column, report.row);
        const auto bytes = InputEncoder::mouse(report, modes());
        if (!bytes.empty()) {
            link->sendInput(bytes);
        }
    };
    CursorGlide::Config glideConfig;
    glideConfig.enabled = smoothCursor;
    CursorGlide cursorGlide(glideConfig);
    Uint64 lastFrame = SDL_GetTicks64();
    while (running) {
        skipText = false;
        fit();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_TEXTINPUT) {
                const auto mod = modifiers(SDL_GetModState());
                if (skipText) {
                    skipText = false;
                }
                else if (!mod.control) {
                    send(InputEncoder::text(event.text.text, mod.alt));
                }
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN && mouseTracking() &&
                     !modifiers(SDL_GetModState()).shift &&
                     mouseButton(event.button.button)) {
                mouseCaptured = true;
                heldButton = *mouseButton(event.button.button);
                lastMouseCell.reset();
                sendMouse(MouseEvent::Type::Press,
                          heldButton,
                          event.button.x,
                          event.button.y,
                          false,
                          false);
            }
            else if (event.type == SDL_MOUSEBUTTONUP && mouseCaptured &&
                     mouseButton(event.button.button)) {
                const auto button = *mouseButton(event.button.button);
                sendMouse(MouseEvent::Type::Release,
                          button,
                          event.button.x,
                          event.button.y,
                          true,
                          false);
                if (button == heldButton) {
                    mouseCaptured = false;
                    heldButton = MouseEvent::Button::None;
                }
            }
            else if (event.type == SDL_MOUSEMOTION && !dragging &&
                     mouseTracking() &&
                     (mouseCaptured || (modes().mouseTracking ==
                                            TerminalModes::MouseTracking::Any &&
                                        !modifiers(SDL_GetModState()).shift))) {
                sendMouse(MouseEvent::Type::Motion,
                          heldButton,
                          event.motion.x,
                          event.motion.y,
                          mouseCaptured,
                          true);
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN &&
                     event.button.button == SDL_BUTTON_LEFT) {
                clipboardError.clear();
                const auto cell = mouseCell(*window,
                                            backend,
                                            model,
                                            event.button.x,
                                            event.button.y,
                                            false);
                const auto mod = modifiers(SDL_GetModState());
                dragging = cell.has_value();
                if (cell) {
                    selectAt(selection,
                             model,
                             *cell,
                             mod.shift,
                             event.button.clicks);
                }
                else {
                    selection.reset();
                }
            }
            else if ((event.type == SDL_MOUSEMOTION && dragging) ||
                     (event.type == SDL_MOUSEBUTTONUP &&
                      event.button.button == SDL_BUTTON_LEFT)) {
                const bool release = event.type == SDL_MOUSEBUTTONUP;
                if (dragging && selection && selection->valid(model)) {
                    const auto cell =
                        mouseCell(*window,
                                  backend,
                                  model,
                                  release ? event.button.x : event.motion.x,
                                  release ? event.button.y : event.motion.y,
                                  true);
                    if (cell) {
                        selection->extend(model, *cell);
                    }
                }
                if (release) {
                    dragging = false;
                }
            }
            else if (event.type == SDL_WINDOWEVENT &&
                     (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                      event.window.event == SDL_WINDOWEVENT_RESIZED)) {
                // No partial cell at the right or bottom edge; maximized, full
                // screen and tiled windows keep what the window manager gives.
                const auto flags = SDL_GetWindowFlags(window.get());
                if (!(flags &
                      (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP |
                       SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
                    WindowSize current, drawable;
                    SDL_GetWindowSize(
                        window.get(), &current.width, &current.height);
                    SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()),
                                              &drawable.width,
                                              &drawable.height);
                    const auto snapped =
                        snappedWindowSize(current,
                                          drawable,
                                          backend.cellWidth(),
                                          backend.cellHeight(),
                                          SdlBackend::scrollbarStrip());
                    if (!(snapped == current)) {
                        SDL_SetWindowSize(
                            window.get(), snapped.width, snapped.height);
                    }
                }
            }
            else if (event.type == SDL_WINDOWEVENT &&
                     (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                      event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
                const bool gained =
                    event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
                if (!gained) {
                    dragging = false;
                    mouseCaptured = false;
                    heldButton = MouseEvent::Button::None;
                }
                const auto report = InputEncoder::focus(gained, modes());
                if (!report.empty()) {
                    link->sendInput(report);
                }
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                if (mouseTracking() && !modifiers(SDL_GetModState()).shift) {
                    int x = 0, y = 0;
                    SDL_GetMouseState(&x, &y);
                    const auto button = amount > 0
                                            ? MouseEvent::Button::WheelUp
                                            : MouseEvent::Button::WheelDown;
                    const int steps =
                        std::clamp(int(std::ceil(std::abs(amount))), 1, 10);
                    for (int i = 0; i < steps; ++i) {
                        sendMouse(
                            MouseEvent::Type::Press, button, x, y, true, false);
                    }
                }
                else if (model.alternateScreen()) {
                    const auto key = amount > 0 ? InputEncoder::Key::Up
                                                : InputEncoder::Key::Down;
                    for (int i = 0;
                         i <
                         int(std::ceil(std::min(10.0, std::abs(amount)) * 3));
                         ++i) {
                        send(InputEncoder::key(key, {}, modes()));
                    }
                }
                else {
                    model.scrollBy(amount * 3);
                }
            }
            else if (event.type == SDL_KEYDOWN) {
                const auto mod = modifiers(SDL_Keymod(event.key.keysym.mod));
                const auto key = event.key.keysym.sym;
                if (mod.control && mod.shift && key == SDLK_q) {
                    running = false;
                }
                else if (mod.control && mod.shift && key == SDLK_v) {
                    if (!event.key.repeat) {
                        std::unique_ptr<char, decltype(&SDL_free)> clipboard(
                            SDL_GetClipboardText(), SDL_free);
                        if (clipboard &&
                            SDL_strlen(clipboard.get()) <= 1024 * 1024) {
                            send(InputEncoder::paste(clipboard.get(), modes()));
                        }
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_c) {
                    if (!event.key.repeat && selection &&
                        selection->valid(model)) {
                        const auto text = selection->text(model);
                        clipboardError =
                            !text ? "copy failed: selection exceeds 1 MiB"
                            : SDL_SetClipboardText(text->c_str()) == 0
                                ? ""
                                : "clipboard copy failed";
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_a) {
                    if (!event.key.repeat) {
                        selection = Selection::all(model);
                        dragging = false;
                    }
                }
                else if (mod.shift &&
                         (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)) {
                    model.scrollBy((key == SDLK_PAGEUP ? 1.0 : -1.0) *
                                   double(model.rows()));
                }
                else if (mod.control && mod.shift && key == SDLK_HOME) {
                    model.scrollToOldest();
                }
                else if (mod.control && mod.shift && key == SDLK_END) {
                    model.followTail();
                }
                else if (modes().applicationKeypad && keypadKey(key)) {
                    skipText = true;
                    send(InputEncoder::keypad(*keypadKey(key), mod, modes()));
                }
                else if (const auto encoded = special(key)) {
                    send(InputEncoder::key(*encoded, mod, modes()));
                }
                else if (mod.control && key >= 0 && key <= 127) {
                    send(InputEncoder::control(char(key), mod.alt));
                }
            }
        }
        if (!running) {
            break;
        }
        link->tick();
        const auto now = SDL_GetTicks64();
        model.advanceViewport(double(now - lastFrame) / 1000);
        lastFrame = now;
        auto frame = model.frame(backend.cellHeight());
        frame.palette = &palette;
        if (selection) {
            if (selection->valid(model)) {
                selection->apply(model, frame);
            }
            else {
                selection.reset();
                dragging = false;
            }
        }
        frame.textBlinkVisible = (now / 500) % 2 == 0;
        // Before blink masking: the glide follows the true cell, not the blink.
        cursorGlide.update(frame.cursor.column,
                           frame.cursor.row,
                           frame.cursor.visible,
                           double(now) / 1000);
        cursorGlide.apply(frame.cursor);
        frame.cursor.focused =
            (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0;
        frame.cursor.visible = frame.cursor.visible &&
                               (!frame.cursor.focused || !frame.cursor.blink ||
                                (now / 500) % 2 == 0);
        std::string shown = title;
        if (!clipboardError.empty()) {
            shown += " | " + clipboardError;
        }
        SDL_SetWindowTitle(window.get(), shown.c_str());
        backend.render(frame);
        backend.present();
        // Animations run at full rate; an idle prompt only needs a blink.
        if (!link->nextDeadline()) {
            SDL_Delay(8);
        }
    }
    SDL_StopTextInput();
    return exitCode;
}
} // namespace

int main(int argc, char **argv) {
    const char *fontOverride = nullptr;
    int fontSize = 18;
    bool smokeTest = false, tty = false, smoothCursor = true, sound = false;
    fakeos::Config pretend;
    pretend.seed =
        std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
    pretend.unixTime =
        std::int64_t(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
    pretend.revealCharsPerSecond = 1250; // About 13 lines a second.
    int firstOption = 1;
    if (argc > 1 && std::string_view(argv[1]) != "-h" &&
        std::string_view(argv[1]).substr(0, 2) != "--") {
        fontOverride = argv[1];
        firstOption = 2;
    }
    for (int i = firstOption; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout, argv[0]);
            return 0;
        }
        else if (arg == "--font-size" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, fontSize);
            if (parsed.ec != std::errc{} || parsed.ptr != end || fontSize < 1 ||
                fontSize > 128) {
                std::cerr
                    << "--font-size requires an integer from 1 to 128 pixels\n";
                return 1;
            }
        }
        else if (arg == "--no-intro") {
            pretend.intro = false;
        }
        else if (arg == "--no-reveal") {
            pretend.revealCharsPerSecond = 0;
        }
        else if (arg == "--no-smooth-cursor") {
            smoothCursor = false;
        }
        else if (arg == "--sound") {
            sound = true;
        }
        else if (arg == "--mute") {
            sound = false;
        }
        else if (arg == "--seed" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            std::uint64_t seed = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), seed);
            if (parsed.ec != std::errc{}) {
                std::cerr << "--seed requires a number\n";
                return 1;
            }
            pretend.seed = seed;
        }
        else if (arg == "--smoke-test") {
            smokeTest = true;
        }
#ifdef MATTERM_DEMO_TTY
        else if (arg == "--tty") {
            tty = true;
        }
#endif
        else {
            usage(std::cerr, argv[0]);
            return 1;
        }
    }
    if (tty && smokeTest) {
        usage(std::cerr, argv[0]);
        return 1;
    }
#ifdef MATTERM_DEMO_TTY
    if (tty) {
        return runTtyHost(pretend);
    }
#endif
    try {
        const auto font = applicationFontPath(fontOverride);
        return runWindow(
            font.c_str(), fontSize, pretend, smokeTest, smoothCursor, sound);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
