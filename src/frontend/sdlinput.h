#pragma once

// SDL event helpers shared by the native terminal and the browser build:
// key and button mapping, text selection, and window-to-cell conversion.

#include "cellhit.h"
#include "inputencoder.h"
#include "sdlbackend.h"
#include "terminalmodel.h"
#include "textselection.h"
#include "urllink.h"

#include <SDL.h>

#include <algorithm>
#include <optional>

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

inline bool copySelection(const Selection &selection,
                          const TerminalModel &model) {
    const auto text = selection.text(model);
    return text && SDL_SetClipboardText(text->c_str()) == 0;
}

// The cell under the mouse (window coordinates). For a pane of a window with
// several, `originX` is the pane's left edge and `paneWidth` its width, both in
// device pixels; the defaults are the whole drawable.
inline std::optional<TerminalModel::TextCell> mouseCell(
    SDL_Window &window,
    const SdlBackend &backend,
    const TerminalModel &model,
    int mouseX,
    int mouseY,
    bool clampToText,
    int originX = 0,
    int paneWidth = 0) {
    int windowWidth = 0, windowHeight = 0;
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetWindowSize(&window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 ||
        SDL_GetRendererOutputSize(
            SDL_GetRenderer(&window), &drawableWidth, &drawableHeight) < 0) {
        return std::nullopt;
    }
    const double x = double(mouseX) * drawableWidth / windowWidth - originX;
    const double y = double(mouseY) * drawableHeight / windowHeight;
    return cellAt(model,
                  x,
                  y,
                  backend.cellWidth(),
                  backend.cellHeight(),
                  paneWidth > 0 ? paneWidth : drawableWidth,
                  drawableHeight,
                  clampToText);
}
// Poll after model/viewport updates so stationary-pointer previews cannot
// retain stale text after output, scrolling or a screen switch.
inline std::optional<UrlMatch> hoveredUrl(SDL_Window &window,
                                          const SdlBackend &backend,
                                          const TerminalModel &model,
                                          bool applicationMouse,
                                          bool captured,
                                          int originX = 0,
                                          int paneWidth = 0) {
    const auto mod = modifiers(SDL_GetModState());
    constexpr auto focus = SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS;
    if (!localLinkGesture(mod.control, mod.shift, applicationMouse, captured) ||
        (SDL_GetWindowFlags(&window) & focus) != focus) {
        return std::nullopt;
    }
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    const auto cell =
        mouseCell(window, backend, model, x, y, false, originX, paneWidth);
    return cell ? urlAt(model, *cell) : std::nullopt;
}
} // namespace frontend
