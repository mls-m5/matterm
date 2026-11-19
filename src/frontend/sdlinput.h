#pragma once

// SDL event helpers shared by the native terminal and the browser build:
// key and button mapping, text selection, and window-to-cell conversion.

#include "inputencoder.h"
#include "sdlbackend.h"
#include "terminalmodel.h"

#include <SDL.h>

#include <algorithm>
#include <optional>
#include <tuple>

namespace frontend {
inline std::optional<InputEncoder::Key> special(SDL_Keycode key) {
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
// Keypad keys that application keypad mode (DECKPAM) reports as SS3 codes.
inline std::optional<char> keypadKey(SDL_Keycode key) {
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
        return char('1' + (key - SDLK_KP_1));
    }
    switch (key) {
    case SDLK_KP_0:
        return '0';
    case SDLK_KP_PERIOD:
        return '.';
    case SDLK_KP_PLUS:
        return '+';
    case SDLK_KP_MINUS:
        return '-';
    case SDLK_KP_MULTIPLY:
        return '*';
    case SDLK_KP_DIVIDE:
        return '/';
    case SDLK_KP_COMMA:
        return ',';
    case SDLK_KP_EQUALS:
        return '=';
    case SDLK_KP_ENTER:
        return '\r';
    default:
        return std::nullopt;
    }
}
inline std::optional<InputEncoder::MouseEvent::Button> mouseButton(
    Uint8 button) {
    using Button = InputEncoder::MouseEvent::Button;
    switch (button) {
    case SDL_BUTTON_LEFT:
        return Button::Left;
    case SDL_BUTTON_MIDDLE:
        return Button::Middle;
    case SDL_BUTTON_RIGHT:
        return Button::Right;
    default:
        return std::nullopt;
    }
}
inline InputEncoder::Modifiers modifiers(SDL_Keymod mod) {
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

inline std::optional<TerminalModel::TextCell> mouseCell(
    SDL_Window &window,
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
} // namespace frontend
