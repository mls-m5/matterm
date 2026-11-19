// Browser front end: the terminal model, VT parser and SDL renderer on a
// canvas, connected to matterm-server through ClientSession (WebSocket). With
// no server URL it runs an interactive terminal simulator (ScriptedLink).
#include "clientsession.h"
#include "cursorglide.h"
#include "inputencoder.h"
#include "palette.h"
#include "scriptedlink.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"
#include "vtparser.h"
#include "websocketchannel.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

using namespace frontend;
using MouseEvent = InputEncoder::MouseEvent;

namespace {
struct App {
    SDL_Window *window = nullptr;
    std::unique_ptr<SdlBackend> backend;
    std::unique_ptr<TerminalModel> model;
    std::unique_ptr<VtParser> parser;
    std::unique_ptr<WebSocketChannel> channel;
    std::unique_ptr<ClientSession> session;
    std::unique_ptr<ScriptedLink> demo; // Simulator when there is no server.
    TerminalLink *link = nullptr;       // session or demo.
    const Palette defaultPalette;
    Palette palette;
    Uint64 last = 0;
    CursorGlide cursorGlide;
    bool skipText = false;
    // The session is over (exit, error or lost connection): Enter reloads the
    // page, which shows the login form again (each login allows one upgrade).
    bool ended = false;
    // Mouse: application reporting, or local selection.
    bool mouseCaptured = false;
    MouseEvent::Button heldButton = MouseEvent::Button::None;
    std::optional<std::pair<int, int>> lastMouseCell;
    bool dragging = false;
    std::optional<Selection> selection;
};

App *gApp = nullptr; // For matterm_paste.

// The `ws` query parameter (?ws=ws://127.0.0.1:8080/ws) or window.mattermWs
// (set by login.js). Empty: demo.
std::string wsUrl() {
    char *value = static_cast<char *>(EM_ASM_PTR({
        const v = new URLSearchParams(location.search).get("ws") ||
                  globalThis.mattermWs || "";
        const n = lengthBytesUTF8(v) + 1;
        const p = _malloc(n);
        stringToUTF8(v, p, n);
        return p;
    }));
    std::string url = value;
    std::free(value);
    return url;
}

// The font is sized in device pixels, so scale it by the page's pixel ratio:
// text keeps its CSS size and is sharp on HiDPI screens.
int fontPixels() {
    const double ratio =
        EM_ASM_DOUBLE({ return window.devicePixelRatio || 1; });
    return std::clamp(int(std::lround(18 * ratio)), 8, 96);
}

void setTitle(const std::string &title) {
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
}

void copyToClipboard(const std::string &text) {
    EM_ASM(
        {
            const t = UTF8ToString($0);
            if (navigator.clipboard) {
                navigator.clipboard.writeText(t).catch(function(){});
            }
        },
        text.c_str());
}

void send(App &app, const std::string &bytes) {
    if (bytes.empty() || !app.link) {
        return;
    }
    if (app.link->sendInput(bytes)) {
        app.model->followTail();
    }
}

bool mouseTracking(const App &app) {
    return app.parser->modes().mouseTracking !=
           TerminalModes::MouseTracking::Off;
}

void sendMouse(App &app,
               MouseEvent::Type type,
               MouseEvent::Button button,
               int x,
               int y,
               bool clamp,
               bool dedupe) {
    auto &model = *app.model;
    const auto cell = mouseCell(*app.window, *app.backend, model, x, y, clamp);
    if (!cell) {
        return;
    }
    const auto first = model.firstRetainedRow() + model.viewport().firstRow();
    MouseEvent report;
    report.type = type;
    report.button = button;
    report.modifiers = modifiers(SDL_GetModState());
    report.column = int(cell->column);
    report.row = cell->row >= first ? int(cell->row - first) : 0;
    if (dedupe &&
        app.lastMouseCell == std::make_pair(report.column, report.row)) {
        return;
    }
    app.lastMouseCell = std::make_pair(report.column, report.row);
    send(app, InputEncoder::mouse(report, app.parser->modes()));
}

void handleKey(App &app, const SDL_KeyboardEvent &key) {
    const auto mod = modifiers(SDL_Keymod(key.keysym.mod));
    const auto sym = key.keysym.sym;
    const auto &modes = app.parser->modes();
    if (mod.control && mod.shift && sym == SDLK_c) {
        // Ctrl+Shift+V (paste) is handled by the page's paste event.
        if (!key.repeat && app.selection && app.selection->valid(*app.model)) {
            if (const auto text = app.model->copyText(
                    app.selection->anchor, app.selection->end, 1024 * 1024)) {
                copyToClipboard(*text);
            }
        }
    }
    else if (mod.control && mod.shift && sym == SDLK_v) {
        // Nothing to send: see above.
    }
    else if (mod.shift && (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN)) {
        app.model->scrollBy((sym == SDLK_PAGEUP ? 1.0 : -1.0) *
                            app.model->rows());
    }
    else if (mod.control && mod.shift && sym == SDLK_HOME) {
        app.model->scrollToOldest();
    }
    else if (mod.control && mod.shift && sym == SDLK_END) {
        app.model->followTail();
    }
    else if (modes.applicationKeypad && keypadKey(sym)) {
        app.skipText = true;
        send(app, InputEncoder::keypad(*keypadKey(sym), mod, modes));
    }
    else if (const auto encoded = special(sym)) {
        send(app, InputEncoder::key(*encoded, mod, modes));
    }
    else if (mod.control && sym >= 0 && sym <= 127) {
        send(app, InputEncoder::control(char(sym), mod.alt));
    }
}

void handleWheel(App &app, const SDL_MouseWheelEvent &wheel) {
    double amount = wheel.preciseY != 0 ? wheel.preciseY : wheel.y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        amount = -amount;
    }
    const auto &modes = app.parser->modes();
    if (mouseTracking(app) && !modifiers(SDL_GetModState()).shift) {
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        const auto button = amount > 0 ? MouseEvent::Button::WheelUp
                                       : MouseEvent::Button::WheelDown;
        const int steps = std::clamp(int(std::ceil(std::abs(amount))), 1, 10);
        for (int i = 0; i < steps; ++i) {
            sendMouse(app, MouseEvent::Type::Press, button, x, y, true, false);
        }
    }
    else if (app.model->alternateScreen()) {
        const auto key =
            amount > 0 ? InputEncoder::Key::Up : InputEncoder::Key::Down;
        const int count = int(std::ceil(std::min(10.0, std::abs(amount)) * 3));
        for (int i = 0; i < count; ++i) {
            send(app, InputEncoder::key(key, {}, modes));
        }
    }
    else {
        app.model->scrollBy(amount * 3);
    }
}

void handleEvent(App &app, const SDL_Event &event) {
    auto &model = *app.model;
    if (app.ended) {
        if (event.type == SDL_KEYDOWN &&
            (event.key.keysym.sym == SDLK_RETURN ||
             event.key.keysym.sym == SDLK_KP_ENTER)) {
            EM_ASM({ location.reload(); });
        }
        return;
    }
    if (event.type == SDL_TEXTINPUT) {
        const auto mod = modifiers(SDL_GetModState());
        if (app.skipText) {
            app.skipText = false;
        }
        else if (!mod.control) {
            send(app, InputEncoder::text(event.text.text, mod.alt));
        }
    }
    else if (event.type == SDL_KEYDOWN) {
        handleKey(app, event.key);
    }
    else if (event.type == SDL_MOUSEBUTTONDOWN && mouseTracking(app) &&
             !modifiers(SDL_GetModState()).shift &&
             mouseButton(event.button.button)) {
        app.mouseCaptured = true;
        app.heldButton = *mouseButton(event.button.button);
        app.lastMouseCell.reset();
        sendMouse(app,
                  MouseEvent::Type::Press,
                  app.heldButton,
                  event.button.x,
                  event.button.y,
                  false,
                  false);
    }
    else if (event.type == SDL_MOUSEBUTTONUP && app.mouseCaptured &&
             mouseButton(event.button.button)) {
        const auto button = *mouseButton(event.button.button);
        sendMouse(app,
                  MouseEvent::Type::Release,
                  button,
                  event.button.x,
                  event.button.y,
                  true,
                  false);
        if (button == app.heldButton) {
            app.mouseCaptured = false;
            app.heldButton = MouseEvent::Button::None;
        }
    }
    else if (event.type == SDL_MOUSEMOTION && mouseTracking(app) &&
             (app.mouseCaptured || (app.parser->modes().mouseTracking ==
                                        TerminalModes::MouseTracking::Any &&
                                    !modifiers(SDL_GetModState()).shift))) {
        sendMouse(app,
                  MouseEvent::Type::Motion,
                  app.heldButton,
                  event.motion.x,
                  event.motion.y,
                  app.mouseCaptured,
                  true);
    }
    else if (event.type == SDL_MOUSEBUTTONDOWN &&
             event.button.button == SDL_BUTTON_LEFT) {
        app.selection.reset();
        const auto cell = mouseCell(*app.window,
                                    *app.backend,
                                    model,
                                    event.button.x,
                                    event.button.y,
                                    false);
        app.dragging = cell.has_value();
        if (cell) {
            app.selection = Selection{*cell, *cell, model.selectionEpoch()};
        }
    }
    else if ((event.type == SDL_MOUSEMOTION && app.dragging) ||
             (event.type == SDL_MOUSEBUTTONUP &&
              event.button.button == SDL_BUTTON_LEFT)) {
        const bool release = event.type == SDL_MOUSEBUTTONUP;
        if (app.dragging && app.selection && app.selection->valid(model)) {
            const auto cell =
                mouseCell(*app.window,
                          *app.backend,
                          model,
                          release ? event.button.x : event.motion.x,
                          release ? event.button.y : event.motion.y,
                          true);
            if (cell) {
                app.selection->end = *cell;
            }
        }
        if (release) {
            app.dragging = false;
        }
    }
    else if (event.type == SDL_WINDOWEVENT &&
             (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
              event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
        const bool gained = event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
        if (!gained) {
            app.dragging = false;
            app.mouseCaptured = false;
            app.heldButton = MouseEvent::Button::None;
        }
        send(app, InputEncoder::focus(gained, app.parser->modes()));
    }
    else if (event.type == SDL_MOUSEWHEEL) {
        handleWheel(app, event.wheel);
    }
}

void frame(void *data) {
    auto &app = *static_cast<App *>(data);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (app.link) {
            handleEvent(app, event);
        }
    }
    if (app.session) {
        app.session->tick();
    }
    if (app.demo) {
        app.demo->tick();
    }
    const auto columns = std::size_t(std::max(1, app.backend->columns(true)));
    const auto rows = std::size_t(std::max(1, app.backend->rows()));
    if (columns != app.model->columns() || rows != app.model->rows()) {
        app.model->resize(columns, rows);
        if (app.link) {
            app.link->resize(int(columns), int(rows));
        }
    }
    const auto now = SDL_GetTicks64();
    app.model->advanceViewport(double(now - app.last) / 1000);
    app.last = now;
    auto rendered = app.model->frame(app.backend->cellHeight());
    rendered.palette = &app.palette;
    if (app.selection) {
        if (app.selection->valid(*app.model)) {
            app.selection->apply(*app.model, rendered);
        }
        else {
            app.selection.reset();
            app.dragging = false;
        }
    }
    rendered.textBlinkVisible = (now / 500) % 2 == 0;
    // Before blink masking: the glide follows the true cell, not the blink.
    app.cursorGlide.update(rendered.cursor.column,
                           rendered.cursor.row,
                           rendered.cursor.visible,
                           double(now) / 1000);
    app.cursorGlide.apply(rendered.cursor);
    rendered.cursor.focused =
        (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    // An inactive window shows a steady hollow cursor.
    rendered.cursor.visible = rendered.cursor.visible &&
                              (!rendered.cursor.focused ||
                               !rendered.cursor.blink || (now / 500) % 2 == 0);
    app.backend->render(rendered);
    app.backend->present();
}
} // namespace

// Called by login.js's paste listener with the clipboard text.
extern "C" EMSCRIPTEN_KEEPALIVE void matterm_paste(const char *text) {
    if (gApp && text) {
        send(*gApp, InputEncoder::paste(text, gApp->parser->modes()));
    }
}

// Soft keyboard and the extra-keys bar (login.js): text from the on-screen
// keyboard, and named keys that phones do not have (Esc, Tab, arrows, ...).
// `mods`: 1 control, 2 alt, 4 shift.
extern "C" EMSCRIPTEN_KEEPALIVE void matterm_text(const char *utf8, int mods) {
    if (!gApp || !utf8 || !*utf8) {
        return;
    }
    const bool control = (mods & 1) != 0, alt = (mods & 2) != 0;
    if (control && std::strlen(utf8) == 1) {
        const auto bytes = InputEncoder::control(utf8[0], alt);
        if (!bytes.empty()) {
            send(*gApp, bytes);
            return;
        }
    }
    send(*gApp, InputEncoder::text(utf8, alt));
}

extern "C" EMSCRIPTEN_KEEPALIVE void matterm_key(const char *name, int mods) {
    using Key = InputEncoder::Key;
    static const std::pair<const char *, Key> keys[] = {
        {"Enter", Key::Enter},
        {"Backspace", Key::Backspace},
        {"Tab", Key::Tab},
        {"Escape", Key::Escape},
        {"ArrowUp", Key::Up},
        {"ArrowDown", Key::Down},
        {"ArrowLeft", Key::Left},
        {"ArrowRight", Key::Right},
        {"Home", Key::Home},
        {"End", Key::End},
        {"Insert", Key::Insert},
        {"Delete", Key::Delete},
        {"PageUp", Key::PageUp},
        {"PageDown", Key::PageDown}};
    if (!gApp || !name) {
        return;
    }
    for (const auto &[keyName, key] : keys) {
        if (std::strcmp(keyName, name) == 0) {
            InputEncoder::Modifiers modifiers;
            modifiers.control = (mods & 1) != 0;
            modifiers.alt = (mods & 2) != 0;
            modifiers.shift = (mods & 4) != 0;
            send(*gApp,
                 InputEncoder::key(key, modifiers, gApp->parser->modes()));
            return;
        }
    }
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::fprintf(stderr, "init failed: %s\n", SDL_GetError());
        return 1;
    }
    static App app;
    gApp = &app;
    app.window =
        SDL_CreateWindow("matterm",
                         SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED,
                         960,
                         600,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!app.window) {
        std::fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }
    try {
        app.backend = std::make_unique<SdlBackend>(
            *app.window, "/assets/font.ttf", fontPixels());
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "backend failed: %s\n", error.what());
        return 1;
    }
    app.model = std::make_unique<TerminalModel>(
        std::size_t(std::max(1, app.backend->columns(true))),
        std::size_t(std::max(1, app.backend->rows())),
        1000);
    VtEvents vtEvents;
    vtEvents.reply = [](std::string_view bytes) {
        if (gApp->link) {
            gApp->link->sendInput(bytes);
        }
    };
    vtEvents.title = [](std::string_view title) {
        setTitle(std::string(title));
    };
    vtEvents.palette = &app.palette;
    vtEvents.setPaletteColor = [](std::size_t index, Rgb8 color) {
        gApp->palette.colors[index] = color;
    };
    vtEvents.resetPaletteColor = [](std::optional<std::size_t> index) {
        if (index) {
            gApp->palette.colors[*index] = gApp->defaultPalette.colors[*index];
        }
        else {
            gApp->palette.colors = gApp->defaultPalette.colors;
        }
    };
    app.parser = std::make_unique<VtParser>(*app.model, std::move(vtEvents));
    // ?smooth glides the cursor between positions (off by default);
    // ?smooth=300 sets the glide time in milliseconds (default 80).
    CursorGlide::Config glide;
    const int smoothMs = EM_ASM_INT({
        const p = new URLSearchParams(location.search);
        if (!p.has("smooth")) {
            return 0;
        }
        const v = parseInt(p.get("smooth"), 10);
        return v > 0 ? v : 80;
    });
    glide.enabled = smoothMs > 0;
    glide.durationSeconds = smoothMs / 1000.0;
    app.cursorGlide = CursorGlide(glide);
    const auto url = wsUrl();
    if (!url.empty()) {
        try {
            app.channel = std::make_unique<WebSocketChannel>(url);
            ClientSession::Config config;
            config.clock = [] {
                return protocol::Time(
                    std::chrono::milliseconds(SDL_GetTicks64()));
            };
            config.columns = int(app.model->columns());
            config.rows = int(app.model->rows());
            app.session = std::make_unique<ClientSession>(*app.channel, config);
            TerminalLink::Events events;
            events.output = [](std::string_view bytes) {
                gApp->parser->feed(bytes);
            };
            events.exited = [](int code, int signal) {
                gApp->ended = true;
                gApp->parser->feed("\r\n[process exited: " +
                                   (signal ? "signal " + std::to_string(signal)
                                           : std::to_string(code)) +
                                   "]\r\n[Press Enter to log in again]\r\n");
            };
            events.closed = [](std::string_view reason) {
                gApp->ended = true;
                gApp->parser->feed("\r\n[disconnected: " + std::string(reason) +
                                   "]\r\n[Press Enter to log in again]\r\n");
            };
            app.session->setEvents(std::move(events));
            app.link = app.session.get();
        }
        catch (const std::exception &error) {
            std::fprintf(stderr, "link failed: %s\n", error.what());
        }
    }
    if (!app.link) {
        // No server: an interactive terminal simulator (docs/SIMULATOR.md).
        ScriptedLink::Config config;
        config.clock = [] {
            return protocol::Time(std::chrono::milliseconds(SDL_GetTicks64()));
        };
        config.columns = int(app.model->columns());
        config.rows = int(app.model->rows());
        app.demo = ScriptedLink::simulator(config);
        app.link = app.demo.get();
        TerminalLink::Events events;
        events.output = [](std::string_view bytes) {
            gApp->parser->feed(bytes);
        };
        events.exited = [](int code, int) {
            gApp->parser->feed("\r\n[process exited: " + std::to_string(code) +
                               "]\r\n");
        };
        app.demo->setEvents(std::move(events));
    }
    SDL_StartTextInput();
    app.last = SDL_GetTicks64();
    emscripten_set_main_loop_arg(frame, &app, 0, 1);
    return 0;
}
