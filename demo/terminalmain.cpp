#include "democontent.h"
#include "fontpath.h"
#include "sdlbackend.h"
#include "terminalmodel.h"
#include "utf8decoder.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void greeting(TerminalModel &model) {
    model.setColors({120, 220, 170}, {14, 17, 22});
    model.eraseDisplay(TerminalModel::EraseMode::All);
    model.moveCursor(0, 0);
    model.write(model.alternateScreen() ? U"matterm - alternate screen\r\n"
                                        : U"matterm - terminal model demo\r\n");
    model.setColors({220, 225, 245}, {14, 17, 22});
    model.write(U"Type text. Enter: CR LF. Arrows: move cursor.\r\n");
    model.write(U"F2: switch primary/alternate   F3: generate rows\r\n");
    model.write(U"F4: erase screen   F5: reset demo   Escape: exit\r\n");
    model.write(U"Wheel / PgUp / PgDn: history   Ctrl+End: follow\r\n");
    model.write(U"UTF-32 API, no VT parser or shell yet.\r\n\r\n> ");
}

void burst(TerminalModel &model, int &number) {
    model.write(U"\r\n");
    for (int i = 0; i < 40; ++i) {
        const auto label = std::to_string(number++);
        model.setColors(i % 2 ? Rgb8{120, 220, 170} : Rgb8{160, 180, 250},
                        {14, 17, 22});
        model.write(U"Generated line ");
        model.write(std::u32string(label.begin(), label.end()));
        model.write(U" | Agjpqy ÅÄÖ åäö\r\n");
    }
    model.setColors({220, 225, 245}, {14, 17, 22});
    model.write(U"> ");
}

void input(TerminalModel &model, const char *utf8) {
    // SDL text events contain complete UTF-8 strings. Decode without iconv;
    // the incremental decoder can later be reused by a streaming VT parser.
    Utf8Decoder decoder;
    model.write(decoder.feed(utf8));
    model.write(decoder.finish());
}

std::vector<Uint32> pixels(SDL_Window &window, const SdlBackend &backend) {
    SDL_Rect area{0, 0, 400, backend.cellHeight()};
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

void smoke(SDL_Window &window, SdlBackend &backend, TerminalModel &model) {
    const auto insertion = model.cursor();
    input(model,
          "aåäö"); // Exercise the same path as SDL_TEXTINPUT, not just drawing.
    const std::u32string expected = U"aåäö";
    for (std::size_t x = 0; x < expected.size(); ++x) {
        if (model
                .cell(std::size_t(insertion.column) + x,
                      std::size_t(insertion.row))
                .codePoint != expected[x]) {
            throw std::runtime_error("Terminal smoke test: UTF-8 input failed");
        }
    }
    greeting(model);
    int number = 1;
    burst(model, number);
    model.scrollToOldest();
    model.advanceViewport(10);
    auto frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    const auto primary = pixels(window, backend);
    if (std::none_of(primary.begin(), primary.end(), [](Uint32 pixel) {
            return (pixel & 0xffffff) != 0x0e1116;
        })) {
        throw std::runtime_error("Terminal smoke test: no glyph pixels");
    }
    backend.present();
    const auto savedCursor = model.cursor();
    const auto savedOffset = model.viewport().offset();
    model.setAlternateScreen(true);
    greeting(model);
    frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    if (pixels(window, backend) == primary || model.historySize() != 0) {
        throw std::runtime_error(
            "Terminal smoke test: alternate screen not independent");
    }
    backend.present();
    burst(model, number);
    if (model.historySize() != 0) {
        throw std::runtime_error(
            "Terminal smoke test: alternate history accumulated");
    }
    model.setAlternateScreen(false);
    frame = model.frame(backend.cellHeight());
    frame.cursor.visible = false;
    backend.render(frame);
    if (pixels(window, backend) != primary ||
        model.viewport().offset() != savedOffset ||
        model.cursor().column != savedCursor.column ||
        model.cursor().row != savedCursor.row) {
        throw std::runtime_error(
            "Terminal smoke test: primary state was not restored");
    }
    backend.present();
    std::cout
        << "Terminal model smoke test passed (primary/alternate restoration)\n";
}

int run(const char *font, bool smokeTest) {
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("matterm-terminal-demo",
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
    // Keep the same column count in both modes so hiding the sidebar itself
    // does not resize or clip the preserved primary screen.
    auto width = [&] {
        return std::size_t(std::max(1, backend.columns(true)));
    };
    auto height = [&] { return std::size_t(std::max(1, backend.rows())); };
    auto model = std::make_unique<TerminalModel>(width(), height(), 256);
    greeting(*model);
    if (smokeTest) {
        smoke(*window, backend, *model);
        return 0;
    }
    SDL_StartTextInput();
    Uint64 lastFrame = SDL_GetTicks64();
    int number = 1;
    bool running = true;
    while (running) {
        model->resize(width(), height());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_TEXTINPUT) {
                model->followTail();
                input(*model, event.text.text);
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                model->scrollBy(amount * 3);
            }
            else if (event.type == SDL_KEYDOWN) {
                const auto cursor = model->cursor();
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_F2:
                    if (!event.key.repeat) {
                        model->setAlternateScreen(!model->alternateScreen());
                        if (model->alternateScreen()) {
                            greeting(*model);
                        }
                    }
                    break;
                case SDLK_F3:
                    if (!event.key.repeat) {
                        burst(*model, number);
                    }
                    break;
                case SDLK_F4:
                    model->eraseDisplay(TerminalModel::EraseMode::All);
                    model->moveCursor(0, 0);
                    model->followTail();
                    break;
                case SDLK_F5:
                    if (!event.key.repeat) {
                        auto fresh = std::make_unique<TerminalModel>(
                            width(), height(), 256);
                        greeting(*fresh);
                        model = std::move(fresh);
                        number = 1;
                    }
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    model->followTail();
                    model->write(U"\r\n");
                    break;
                case SDLK_BACKSPACE:
                    if (cursor.column > 0) {
                        model->write(U"\b \b");
                    }
                    break;
                case SDLK_TAB:
                    model->write(U"\t");
                    break;
                case SDLK_LEFT:
                    model->moveCursor(cursor.column - 1, cursor.row);
                    break;
                case SDLK_RIGHT:
                    model->moveCursor(cursor.column + 1, cursor.row);
                    break;
                case SDLK_UP:
                    model->moveCursor(cursor.column, cursor.row - 1);
                    break;
                case SDLK_DOWN:
                    model->moveCursor(cursor.column, cursor.row + 1);
                    break;
                case SDLK_PAGEUP:
                    model->scrollBy(double(model->rows()));
                    break;
                case SDLK_PAGEDOWN:
                    model->scrollBy(-double(model->rows()));
                    break;
                case SDLK_HOME:
                    if (event.key.keysym.mod & KMOD_CTRL) {
                        model->scrollToOldest();
                    }
                    else {
                        model->moveCursor(0, cursor.row);
                    }
                    break;
                case SDLK_END:
                    if (event.key.keysym.mod & KMOD_CTRL) {
                        model->followTail();
                    }
                    else {
                        model->moveCursor(int(model->columns()) - 1,
                                          cursor.row);
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (!running) {
            break;
        }
        const auto now = SDL_GetTicks64();
        model->advanceViewport(double(now - lastFrame) / 1000);
        lastFrame = now;
        auto frame = model->frame(backend.cellHeight());
        frame.cursor.visible = frame.cursor.visible && (now / 500) % 2 == 0;
        const auto title =
            std::string("matterm-terminal-demo | F2: switch | F3: rows | ") +
            (model->alternateScreen() ? "alternate (no history)" : "primary") +
            " | history " + std::to_string(model->historySize());
        SDL_SetWindowTitle(window.get(), title.c_str());
        backend.render(frame);
        backend.present();
    }
    SDL_StopTextInput();
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    demo::printBanner(std::cout);
    const char *fontOverride = nullptr;
    bool smokeTest = false;
    bool valid = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--smoke-test") {
            smokeTest = true;
        }
        else if (!fontOverride &&
                 std::string_view(argv[i]).substr(0, 2) != "--") {
            fontOverride = argv[i];
        }
        else {
            valid = false;
        }
    }
    if (!valid) {
        std::cerr << "Usage: " << argv[0] << " [font.ttf] [--smoke-test]\n";
        return 1;
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
        result = run(font.c_str(), smokeTest);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    TTF_Quit();
    SDL_Quit();
    return result;
}
