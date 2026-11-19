#include "ptyfrontend.h"

#include "inputencoder.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "terminalmodel.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace {
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

std::optional<InputEncoder::Key> special(SDL_Keycode key) {
    using Key = InputEncoder::Key;
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return Key::Enter;
    case SDLK_BACKSPACE:
        return Key::Backspace;
    case SDLK_TAB:
        return Key::Tab;
    case SDLK_ESCAPE:
        return Key::Escape;
    case SDLK_UP:
        return Key::Up;
    case SDLK_DOWN:
        return Key::Down;
    case SDLK_LEFT:
        return Key::Left;
    case SDLK_RIGHT:
        return Key::Right;
    case SDLK_HOME:
        return Key::Home;
    case SDLK_END:
        return Key::End;
    case SDLK_INSERT:
        return Key::Insert;
    case SDLK_DELETE:
        return Key::Delete;
    case SDLK_PAGEUP:
        return Key::PageUp;
    case SDLK_PAGEDOWN:
        return Key::PageDown;
    case SDLK_F1:
        return Key::F1;
    case SDLK_F2:
        return Key::F2;
    case SDLK_F3:
        return Key::F3;
    case SDLK_F4:
        return Key::F4;
    case SDLK_F5:
        return Key::F5;
    case SDLK_F6:
        return Key::F6;
    case SDLK_F7:
        return Key::F7;
    case SDLK_F8:
        return Key::F8;
    case SDLK_F9:
        return Key::F9;
    case SDLK_F10:
        return Key::F10;
    case SDLK_F11:
        return Key::F11;
    case SDLK_F12:
        return Key::F12;
    default:
        return std::nullopt;
    }
}
InputEncoder::Modifiers modifiers(SDL_Keymod mod) {
    const bool altGr = (mod & KMOD_RALT) != 0;
    return {bool(mod & KMOD_SHIFT),
            bool(mod & KMOD_ALT) && !altGr,
            bool(mod & KMOD_CTRL) && !altGr};
}

struct Selection {
    TerminalModel::TextCell anchor, end;
    std::size_t epoch;

    bool valid(const TerminalModel &model) const {
        return epoch == model.selectionEpoch() &&
               std::min(anchor.row, end.row) >= model.firstRetainedRow();
    }

    bool copy(const TerminalModel &model) const {
        const auto text = model.copyText(anchor, end, 1024 * 1024);
        return text && SDL_SetClipboardText(text->c_str()) == 0;
    }

    void apply(const TerminalModel &model, RenderFrame &frame) const {
        if (!valid(model)) {
            return;
        }
        auto first = anchor;
        auto last = end;
        if (std::tie(last.row, last.column) <
            std::tie(first.row, first.column)) {
            std::swap(first, last);
        }
        const auto viewFirst =
            model.firstRetainedRow() + model.viewport().firstRow();
        const auto viewEnd = viewFirst + frame.cells.rows();
        if (last.row < viewFirst || first.row >= viewEnd) {
            return;
        }
        const auto columns = frame.cells.columns();
        frame.selectionBegin =
            first.row < viewFirst
                ? 0
                : (first.row - viewFirst) * columns + first.column;
        frame.selectionEnd =
            last.row >= viewEnd
                ? frame.cells.rows() * columns
                : (last.row - viewFirst) * columns + last.column + 1;
    }
};

std::optional<TerminalModel::TextCell> mouseCell(SDL_Window &window,
                                                 const SdlBackend &backend,
                                                 const TerminalModel &model,
                                                 int mouseX,
                                                 int mouseY,
                                                 bool clampToText) {
    int windowWidth = 0, windowHeight = 0;
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetWindowSize(&window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 ||
        SDL_GetRendererOutputSize(
            SDL_GetRenderer(&window), &drawableWidth, &drawableHeight) < 0) {
        return std::nullopt;
    }
    const auto &viewport = model.viewport();
    const int cellWidth = backend.cellWidth();
    const int cellHeight = backend.cellHeight();
    const double x = double(mouseX) * drawableWidth / windowWidth;
    const double y = double(mouseY) * drawableHeight / windowHeight;
    const double textWidth =
        std::min(double(drawableWidth), double(model.columns()) * cellWidth);
    const double textHeight =
        std::min(double(drawableHeight), double(model.rows()) * cellHeight);
    if (textWidth < 1 || textHeight < 1 || !viewport.rowCount() ||
        (!clampToText &&
         (x < 0 || x >= textWidth || y < 0 || y >= textHeight))) {
        return std::nullopt;
    }
    const auto column =
        std::size_t(std::clamp(x, 0.0, textWidth - 1) / cellWidth);
    const auto row = std::size_t((std::clamp(y, 0.0, textHeight - 1) -
                                  viewport.pixelOffset(cellHeight)) /
                                 cellHeight);
    return TerminalModel::TextCell{model.firstRetainedRow() +
                                       viewport.firstRow() +
                                       std::min(row, viewport.rowCount() - 1),
                                   column};
}

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
        if (!selection.copy(model)) {
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
                   bool smokeTest) {
    std::string title = "matterm";
    // fork/exec before SDL or graphics workers exist. Window dimensions follow
    // immediately after the backend computes drawable-pixel cell metrics.
    PtySession session(arguments,
                       80,
                       24,
                       {{},
                        [&](std::string_view value) {
                            title = "matterm | " + std::string(value);
                        }},
                       term,
                       PtyProcess::ColorCapability::TrueColor);
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
    bool clipboardError = false;
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
    Uint64 lastFrame = SDL_GetTicks64();
    while (running) {
        resize();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_TEXTINPUT) {
                const auto mod = modifiers(SDL_GetModState());
                if (!mod.control) {
                    send(InputEncoder::text(event.text.text, mod.alt));
                }
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN &&
                     event.button.button == SDL_BUTTON_LEFT) {
                selection.reset();
                clipboardError = false;
                const auto cell = mouseCell(*window,
                                            backend,
                                            model,
                                            event.button.x,
                                            event.button.y,
                                            false);
                dragging = cell.has_value();
                if (cell) {
                    selection = Selection{*cell, *cell, model.selectionEpoch()};
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
                        selection->end = *cell;
                    }
                }
                if (event.type == SDL_MOUSEBUTTONUP) {
                    dragging = false;
                }
            }
            else if (event.type == SDL_WINDOWEVENT &&
                     event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                dragging = false;
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                if (session.model().alternateScreen()) {
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
                        clipboardError = !selection->copy(model);
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
        frame.cursor.visible = frame.cursor.visible &&
                               (!frame.cursor.blink || (now / 500) % 2 == 0);
        std::string currentTitle = title;
        if (rejectedInput) {
            currentTitle += " | input queue full/closed";
        }
        if (clipboardError) {
            currentTitle += " | clipboard copy failed";
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
