#include "ptyfrontend.h"

#include "cursorglide.h"
#include "inputencoder.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <unistd.h>

namespace {
using namespace frontend;
class OutputRecording {
public:
    explicit OutputRecording(const char *path) {
        if (!path) {
            return;
        }
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            const int error = errno;
            throw std::system_error(
                error,
                std::generic_category(),
                std::string("Could not create PTY recording: ") + path);
        }
        std::cerr << "Recording PTY output to " << path << '\n';
    }
    ~OutputRecording() {
        close();
    }
    OutputRecording(const OutputRecording &) = delete;
    OutputRecording &operator=(const OutputRecording &) = delete;

    void append(std::string_view bytes) {
        const bool truncated = bytes.size() > remaining_;
        bytes = bytes.substr(0, remaining_);
        while (fd_ >= 0 && !bytes.empty()) {
            const auto count = ::write(fd_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                const std::error_code error(count < 0 ? errno : EIO,
                                            std::generic_category());
                std::cerr << "PTY recording stopped: " << error.message()
                          << '\n';
                close();
                return;
            }
            remaining_ -= std::size_t(count);
            bytes.remove_prefix(std::size_t(count));
        }
        if (fd_ >= 0 && truncated) {
            std::cerr << "PTY recording stopped at 64 MiB; the terminal "
                         "session continues\n";
            close();
        }
    }

private:
    void close() {
        if (fd_ >= 0) {
            if (::close(fd_) < 0) {
                const std::error_code error(errno, std::generic_category());
                std::cerr << "PTY recording close failed: " << error.message()
                          << '\n';
            }
            fd_ = -1;
        }
    }
    int fd_ = -1;
    std::size_t remaining_ = 64 * 1024 * 1024;
};

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

void smoke(SDL_Window &window, SdlBackend &backend, PtySession &session) {
    const auto deadline = SDL_GetTicks64() + 5000;
    while (!session.finished() && SDL_GetTicks64() < deadline) {
        session.pump();
        SDL_Delay(1);
    }
    if (!session.finished() || session.exitStatus()->code != 0) {
        throw std::runtime_error("PTY GUI smoke: child failed or timed out");
    }
    auto &model = session.model();
    if (model.cell(0, 0).codePoint != U'm' ||
        Palette{}.resolve(model.cell(0, 0)).foreground.g != 154 ||
        model.cell(0, 1).codePoint != U'R') {
        throw std::runtime_error(
            "PTY GUI smoke: parsed child output/reply missing");
    }
    auto frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    const SDL_Rect rect{0, 0, 200, backend.cellHeight()};
    std::vector<Uint32> pixels(std::size_t(rect.w) * rect.h);
    if (SDL_RenderReadPixels(SDL_GetRenderer(&window),
                             &rect,
                             SDL_PIXELFORMAT_ARGB8888,
                             pixels.data(),
                             rect.w * int(sizeof(Uint32))) < 0) {
        throw std::runtime_error(SDL_GetError());
    }
    if (std::none_of(pixels.begin(), pixels.end(), [](Uint32 pixel) {
            return ((pixel >> 8) & 255) > ((pixel >> 16) & 255);
        })) {
        throw std::runtime_error("PTY GUI smoke: no green glyphs rendered");
    }
    backend.present();

    const auto start = mouseCell(window, backend, model, 0, 0, false);
    const auto end =
        mouseCell(window, backend, model, backend.cellWidth() * 3, 0, false);
    if (!start || !end) {
        throw std::runtime_error(
            "PTY GUI smoke: could not locate selection cells");
    }
    const Selection selection{*start, *end, model.selectionEpoch()};
    selection.apply(model, frame);
    if (frame.selectionBegin != 0 || frame.selectionEnd != 4) {
        throw std::runtime_error("PTY GUI smoke: wrong selection range");
    }
    // Only exercise SDL's in-process test clipboard; don't overwrite a user's
    // desktop clipboard when running this smoke test on a real display.
    if (std::string_view(SDL_GetCurrentVideoDriver()) == "dummy") {
        if (!copySelection(selection, model)) {
            throw std::runtime_error("PTY GUI smoke: copy failed");
        }
        const std::unique_ptr<char, decltype(&SDL_free)> clipboard(
            SDL_GetClipboardText(), SDL_free);
        if (!clipboard || std::string_view(clipboard.get()) != "matt") {
            throw std::runtime_error("PTY GUI smoke: wrong clipboard text");
        }
    }
    backend.render(frame);
    if (SDL_RenderReadPixels(SDL_GetRenderer(&window),
                             &rect,
                             SDL_PIXELFORMAT_ARGB8888,
                             pixels.data(),
                             rect.w * int(sizeof(Uint32))) < 0 ||
        std::none_of(pixels.begin(), pixels.end(), [](Uint32 pixel) {
            return (pixel & 0xffffff) == 0x315691;
        })) {
        throw std::runtime_error(
            "PTY GUI smoke: no selection highlight rendered");
    }
    std::cout << "PTY -> VT -> model -> SDL smoke test passed\n";
}
} // namespace

int runPtyFrontend(const char *font,
                   int fontSize,
                   const std::vector<std::string> &arguments,
                   const std::string &term,
                   bool smokeTest,
                   const char *recordPath,
                   bool smoothCursor) {
    OutputRecording recording(recordPath);
    std::string title = "matterm";
    const Palette defaultPalette;
    Palette palette;
    // fork/exec before SDL or graphics workers exist. Window dimensions follow
    // immediately after the backend computes drawable-pixel cell metrics.
    PtySession session(
        arguments,
        80,
        24,
        {{},
         [&](std::string_view value) {
             title = "matterm | " + std::string(value);
         },
         &palette,
         [&](std::size_t index, Rgb8 color) { palette.colors[index] = color; },
         [&](std::optional<std::size_t> index) {
             if (index) {
                 palette.colors[*index] = defaultPalette.colors[*index];
             }
             else {
                 palette.colors = defaultPalette.colors;
             }
         }},
        term,
        PtyProcess::ColorCapability::TrueColor);
    if (recordPath) {
        session.setOutputObserver(
            [&](std::string_view bytes) { recording.append(bytes); });
    }
    SdlRuntime runtime;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("matterm",
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
    auto resize = [&] {
        const int columns = std::clamp(backend.columns(true), 1, 65535);
        const int rows = std::clamp(backend.rows(), 1, 65535);
        if (session.model().columns() != std::size_t(columns) ||
            session.model().rows() != std::size_t(rows)) {
            session.resize(columns, rows);
        }
    };
    resize();
    if (smokeTest) {
        smoke(*window, backend, session);
        return 0;
    }
    SDL_StartTextInput();
    bool running = true;
    bool rejectedInput = false;
    std::string clipboardError;
    std::string linkError;
    bool dragging = false;
    std::optional<Selection> selection;
    auto &model = session.model();
    const auto send = [&](const std::string &bytes) {
        if (bytes.empty() || session.finished()) {
            return;
        }
        rejectedInput = !session.send(bytes);
        if (!rejectedInput) {
            session.model().followTail();
        }
    };
    // Application mouse reporting. Shift hands the mouse back to selection
    // unless a reported press is still held.
    using MouseEvent = InputEncoder::MouseEvent;
    bool mouseCaptured = false;
    MouseEvent::Button heldButton = MouseEvent::Button::None;
    std::optional<std::pair<int, int>> lastMouseCell;
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
        const auto bytes = InputEncoder::mouse(report, session.modes());
        if (!bytes.empty() && !session.finished()) {
            rejectedInput = !session.send(bytes);
        }
    };
    const auto mouseTracking = [&] {
        return session.modes().mouseTracking !=
               TerminalModes::MouseTracking::Off;
    };
    CursorGlide::Config glideConfig;
    glideConfig.enabled = smoothCursor;
    CursorGlide cursorGlide(glideConfig);
    bool skipText = false;
    Uint64 lastFrame = SDL_GetTicks64();
    while (running) {
        skipText = false;
        resize();
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
                     (mouseCaptured || (session.modes().mouseTracking ==
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
                const auto link = cell && localLinkGesture(mod.control,
                                                           mod.shift,
                                                           mouseTracking(),
                                                           mouseCaptured)
                                      ? urlAt(model, *cell)
                                      : std::nullopt;
                if (link) {
                    dragging = false;
                    linkError =
                        SDL_OpenURL(link->text.c_str()) == 0
                            ? ""
                            : std::string("open URL failed: ") + SDL_GetError();
                }
                else {
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
            }
            else if ((event.type == SDL_MOUSEMOTION && dragging) ||
                     (event.type == SDL_MOUSEBUTTONUP &&
                      event.button.button == SDL_BUTTON_LEFT)) {
                if (dragging && selection && selection->valid(model)) {
                    const bool release = event.type == SDL_MOUSEBUTTONUP;
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
                if (event.type == SDL_MOUSEBUTTONUP) {
                    dragging = false;
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
                const auto report =
                    InputEncoder::focus(gained, session.modes());
                if (!report.empty() && !session.finished()) {
                    rejectedInput = !session.send(report);
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
                    const auto button =
                        event.wheel.x != 0 && event.wheel.y == 0
                            ? (amount > 0 ? MouseEvent::Button::WheelRight
                                          : MouseEvent::Button::WheelLeft)
                        : amount > 0 ? MouseEvent::Button::WheelUp
                                     : MouseEvent::Button::WheelDown;
                    if (button == MouseEvent::Button::WheelLeft ||
                        button == MouseEvent::Button::WheelRight) {
                        amount = event.wheel.preciseX != 0
                                     ? event.wheel.preciseX
                                     : event.wheel.x;
                    }
                    const int steps =
                        std::clamp(int(std::ceil(std::abs(amount))), 1, 10);
                    for (int i = 0; i < steps; ++i) {
                        sendMouse(
                            MouseEvent::Type::Press, button, x, y, true, false);
                    }
                }
                else if (session.model().alternateScreen()) {
                    const auto key = amount > 0 ? InputEncoder::Key::Up
                                                : InputEncoder::Key::Down;
                    for (int i = 0;
                         i <
                         int(std::ceil(std::min(10.0, std::abs(amount)) * 3));
                         ++i) {
                        send(InputEncoder::key(key, {}, session.modes()));
                    }
                }
                else {
                    session.model().scrollBy(amount * 3);
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
                            send(InputEncoder::paste(clipboard.get(),
                                                     session.modes()));
                        }
                        else {
                            rejectedInput = true;
                        }
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_c) {
                    if (!event.key.repeat && selection &&
                        selection->valid(model)) {
                        const auto text = selection->text(model);
                        if (!text) {
                            clipboardError =
                                "copy failed: selection exceeds 1 MiB";
                        }
                        else {
                            clipboardError =
                                SDL_SetClipboardText(text->c_str()) == 0
                                    ? ""
                                    : "clipboard copy failed";
                        }
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_a) {
                    if (!event.key.repeat) {
                        selection = Selection::all(model);
                        dragging = false;
                        clipboardError.clear();
                    }
                }
                else if (mod.shift &&
                         (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)) {
                    session.model().scrollBy((key == SDLK_PAGEUP ? 1.0 : -1.0) *
                                             session.model().rows());
                }
                else if (mod.control && mod.shift && key == SDLK_HOME) {
                    session.model().scrollToOldest();
                }
                else if (mod.control && mod.shift && key == SDLK_END) {
                    session.model().followTail();
                }
                else if (session.modes().applicationKeypad && keypadKey(key)) {
                    skipText = true;
                    send(InputEncoder::keypad(
                        *keypadKey(key), mod, session.modes()));
                }
                else if (const auto encoded = special(key)) {
                    send(InputEncoder::key(*encoded, mod, session.modes()));
                }
                else if (mod.control && key >= 0 && key <= 127) {
                    send(InputEncoder::control(char(key), mod.alt));
                }
            }
        }
        if (!running) {
            break;
        }
        session.pump();
        // Wait for both child exit and PTY EOF, so pump can drain final output.
        if (session.finished()) {
            break;
        }
        const auto now = SDL_GetTicks64();
        session.model().advanceViewport(double(now - lastFrame) / 1000);
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
        const auto link =
            hoveredUrl(*window, backend, model, mouseTracking(), mouseCaptured);
        if (link) {
            link->underline(model, frame);
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
        // An inactive window shows a steady hollow cursor.
        frame.cursor.visible = frame.cursor.visible &&
                               (!frame.cursor.focused || !frame.cursor.blink ||
                                (now / 500) % 2 == 0);
        std::string currentTitle = title;
        if (rejectedInput) {
            currentTitle += " | input queue full/closed";
        }
        if (!clipboardError.empty()) {
            currentTitle += " | " + clipboardError;
        }
        if (!linkError.empty()) {
            currentTitle += " | " + linkError;
        }
        if (link) {
            currentTitle += " | " + link->text;
        }
        SDL_SetWindowTitle(window.get(), currentTitle.c_str());
        backend.render(frame);
        backend.present();
    }
    SDL_StopTextInput();
    return session.exitStatus() && session.exitStatus()->signal == 0
               ? std::max(0, session.exitStatus()->code)
               : 0;
}
