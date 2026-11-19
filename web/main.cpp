// Browser spike: the terminal model, VT parser, and SDL renderer running on a
// canvas, fed with a built-in VT stream. The network link comes later.
#include "sdlbackend.h"
#include "terminalmodel.h"
#include "vtparser.h"
#include "clientsession.h"
#include "inputencoder.h"
#include "websocketchannel.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <emscripten.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace {
struct App {
    SDL_Window *window = nullptr;
    std::unique_ptr<SdlBackend> backend;
    std::unique_ptr<TerminalModel> model;
    std::unique_ptr<VtParser> parser;
    std::unique_ptr<WebSocketChannel> channel;
    std::unique_ptr<ClientSession> session;
    Uint64 last = 0;
    std::string status;
    bool skipText = false;
};

std::optional<InputEncoder::Key> special(SDL_Keycode key) {
    using K = InputEncoder::Key;
    switch (key) {
    case SDLK_RETURN: return K::Enter;
    case SDLK_BACKSPACE: return K::Backspace;
    case SDLK_TAB: return K::Tab;
    case SDLK_ESCAPE: return K::Escape;
    case SDLK_UP: return K::Up;
    case SDLK_DOWN: return K::Down;
    case SDLK_LEFT: return K::Left;
    case SDLK_RIGHT: return K::Right;
    case SDLK_HOME: return K::Home;
    case SDLK_END: return K::End;
    case SDLK_INSERT: return K::Insert;
    case SDLK_DELETE: return K::Delete;
    case SDLK_PAGEUP: return K::PageUp;
    case SDLK_PAGEDOWN: return K::PageDown;
    case SDLK_F1: return K::F1;
    case SDLK_F2: return K::F2;
    case SDLK_F3: return K::F3;
    case SDLK_F4: return K::F4;
    case SDLK_F5: return K::F5;
    case SDLK_F6: return K::F6;
    case SDLK_F7: return K::F7;
    case SDLK_F8: return K::F8;
    case SDLK_F9: return K::F9;
    case SDLK_F10: return K::F10;
    case SDLK_F11: return K::F11;
    case SDLK_F12: return K::F12;
    default: return std::nullopt;
    }
}

// The `ws` query parameter, e.g. ?ws=ws://127.0.0.1:8080/tty. Empty: demo.
std::string wsUrl() {
    char *value = static_cast<char *>(EM_ASM_PTR({
        const v = new URLSearchParams(location.search).get("ws") || "";
        const n = lengthBytesUTF8(v) + 1;
        const p = _malloc(n);
        stringToUTF8(v, p, n);
        return p;
    }));
    std::string url = value;
    std::free(value);
    return url;
}

void frame(void *data) {
    auto &app = *static_cast<App *>(data);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (!app.session) {
            continue;
        }
        const auto &modes = app.parser->modes();
        if (event.type == SDL_TEXTINPUT) {
            if (app.skipText) {
                app.skipText = false;
            }
            else {
                const auto alt = (SDL_GetModState() & KMOD_ALT) != 0;
                app.session->sendInput(
                    InputEncoder::text(event.text.text, alt));
            }
        }
        else if (event.type == SDL_KEYDOWN) {
            const auto m = SDL_GetModState();
            InputEncoder::Modifiers mod;
            mod.shift = (m & KMOD_SHIFT) != 0;
            mod.alt = (m & KMOD_ALT) != 0;
            mod.control = (m & KMOD_CTRL) != 0;
            const auto key = event.key.keysym.sym;
            if (const auto encoded = special(key)) {
                app.session->sendInput(InputEncoder::key(*encoded, mod, modes));
            }
            else if (mod.control && key >= 0 && key <= 127) {
                app.session->sendInput(
                    InputEncoder::control(char(key), mod.alt));
            }
        }
    }
    if (app.session) {
        app.session->tick();
    }
    const auto columns = std::size_t(std::max(1, app.backend->columns()));
    const auto rows = std::size_t(std::max(1, app.backend->rows()));
    if (columns != app.model->columns() || rows != app.model->rows()) {
        app.model->resize(columns, rows);
        if (app.session) {
            app.session->resize(int(columns), int(rows));
        }
    }
    const auto now = SDL_GetTicks64();
    app.model->advanceViewport(double(now - app.last) / 1000);
    app.last = now;
    auto rendered = app.model->frame(app.backend->cellHeight());
    rendered.textBlinkVisible = (now / 500) % 2 == 0;
    rendered.cursor.focused = true;
    rendered.cursor.visible = rendered.cursor.visible && (now / 500) % 2 == 0;
    app.backend->render(rendered);
    app.backend->present();
}
} // namespace

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        std::fprintf(stderr, "init failed: %s\n", SDL_GetError());
        return 1;
    }
    static App app;
    app.window = SDL_CreateWindow("matterm",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  960,
                                  600,
                                  SDL_WINDOW_RESIZABLE);
    if (!app.window) {
        std::fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }
    try {
        app.backend =
            std::make_unique<SdlBackend>(*app.window, "/assets/font.ttf", 18);
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "backend failed: %s\n", error.what());
        return 1;
    }
    app.model = std::make_unique<TerminalModel>(
        std::size_t(std::max(1, app.backend->columns())),
        std::size_t(std::max(1, app.backend->rows())),
        1000);
    app.parser = std::make_unique<VtParser>(*app.model);
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
            app.session =
                std::make_unique<ClientSession>(*app.channel, config);
            TerminalLink::Events events;
            events.output = [](std::string_view bytes) {
                app.parser->feed(bytes);
            };
            events.exited = [](int code, int) {
                app.parser->feed("\r\n[process exited: " +
                                 std::to_string(code) + "]\r\n");
            };
            events.closed = [](std::string_view reason) {
                app.parser->feed("\r\n[disconnected: " +
                                 std::string(reason) + "]\r\n");
            };
            app.session->setEvents(std::move(events));
        }
        catch (const std::exception &error) {
            std::fprintf(stderr, "link failed: %s\n", error.what());
        }
    }
    if (!app.session) app.parser->feed(
        "\x1b[1;32mmatterm\x1b[0m in the browser \x1b[38;5;208m(Emscripten + "
        "SDL2)\x1b[0m\r\n\r\n"
        "\x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m "
        "\x1b[34mblue\x1b[0m \x1b[7minverse\x1b[0m \x1b[4munderline\x1b[0m\r\n"
        "\x1b[48;2;40;80;160m  truecolor background  \x1b[0m\r\n"
        "\x1b(0lqqqqqqqqqqqqqqqk\r\nx\x1b(B Line graphics \x1b(0x\r\n"
        "mqqqqqqqqqqqqqqqj\x1b(B\r\n\r\n$ ");
    app.last = SDL_GetTicks64();
    emscripten_set_main_loop_arg(frame, &app, 0, 1);
    return 0;
}
