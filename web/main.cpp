// Browser spike: the terminal model, VT parser, and SDL renderer running on a
// canvas, fed with a built-in VT stream. The network link comes later.
#include "sdlbackend.h"
#include "terminalmodel.h"
#include "vtparser.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <emscripten.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {
struct App {
    SDL_Window *window = nullptr;
    std::unique_ptr<SdlBackend> backend;
    std::unique_ptr<TerminalModel> model;
    std::unique_ptr<VtParser> parser;
    Uint64 last = 0;
};

void frame(void *data) {
    auto &app = *static_cast<App *>(data);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
    const auto columns = std::size_t(std::max(1, app.backend->columns()));
    const auto rows = std::size_t(std::max(1, app.backend->rows()));
    if (columns != app.model->columns() || rows != app.model->rows()) {
        app.model->resize(columns, rows);
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
    app.parser->feed(
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
