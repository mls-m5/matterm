#include "ptyfrontend.h"

#include "cellbuffer.h"
#include "cursorglide.h"
#include "inputencoder.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"
#include "ptylink.h"
#include "terminalview.h"
#include "workspaceview.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <functional>
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

// Sits between a PtyLink and its TerminalView to copy the raw output (--record-output).
class TapLink final : public TerminalLink {
public:
    TapLink(PtyLink &inner, std::function<void(std::string_view)> tap)
        : inner_(inner)
        , tap_(std::move(tap)) {}
    void setEvents(Events events) override {
        if (events.output) {
            events.output = [this, forward = std::move(events.output)](
                                std::string_view bytes) {
                tap_(bytes);
                forward(bytes);
            };
        }
        inner_.setEvents(std::move(events));
    }
    bool sendInput(std::string_view bytes) override {
        return inner_.sendInput(bytes);
    }
    void resize(int columns, int rows, int pw, int ph) override {
        inner_.resize(columns, rows, pw, ph);
    }
    void close() override {
        inner_.close();
    }

private:
    PtyLink &inner_;
    std::function<void(std::string_view)> tap_;
};

// One terminal of the window: a process and the view of its output. The link is
// declared first because the view borrows it.
struct NativePane {
    PtyLink link;
    std::unique_ptr<TapLink> tap;
    TerminalView v;
    NativePane(const std::vector<std::string> &arguments,
               const std::string &term,
               int columns,
               int rows,
               std::function<void(std::string_view)> record)
        : link(arguments,
               columns,
               rows,
               term,
               PtyProcess::ColorCapability::TrueColor)
        , v(TerminalViewConfig{std::size_t(columns), std::size_t(rows), 1000, {}}) {
        if (record) {
            tap = std::make_unique<TapLink>(link, std::move(record));
            v.attach(*tap);
        }
        else {
            v.attach(link);
        }
    }
    TerminalView &view() {
        return v;
    }
    const TerminalView &view() const {
        return v;
    }
};

using Space = Workspace<NativePane>;

struct CursorDeleter {
    void operator()(SDL_Cursor *c) const {
        SDL_FreeCursor(c);
    }
};
using CursorPtr = std::unique_ptr<SDL_Cursor, CursorDeleter>;

int runSmokeTest(const char *font,
                 int fontSize,
                 const std::vector<std::string> &arguments,
                 const std::string &term) {
    PtySession session(arguments,
                       80,
                       24,
                       {},
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
                             SDL_WINDOW_HIDDEN),
        SDL_DestroyWindow);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SdlBackend backend(*window, font, fontSize, true);
    backend.setReserveScrollbarStrip(true);
    session.resize(std::clamp(backend.columns(true), 1, 65535),
                   std::clamp(backend.rows(), 1, 65535));
    smoke(*window, backend, session);
    return 0;
}

int keyCodeOf(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP:
        return keys::Up;
    case SDLK_DOWN:
        return keys::Down;
    case SDLK_LEFT:
        return keys::Left;
    case SDLK_RIGHT:
        return keys::Right;
    case SDLK_PAGEUP:
        return keys::PageUp;
    case SDLK_PAGEDOWN:
        return keys::PageDown;
    case SDLK_ESCAPE:
        return keys::Escape;
    case SDLK_RETURN:
        return keys::Enter;
    case SDLK_TAB:
        return keys::Tab;
    case SDLK_BACKSPACE:
        return keys::Backspace;
    default:
        return key >= 32 && key < 127 ? int(key) : 0;
    }
}
} // namespace

int runPtyFrontend(const char *font,
                   int fontSize,
                   const std::vector<std::string> &arguments,
                   const std::string &term,
                   bool smokeTest,
                   const char *recordPath,
                   bool smoothCursor,
                   bool tabsSmoke) {
    if (smokeTest) {
        return runSmokeTest(font, fontSize, arguments, term);
    }
    OutputRecording recording(recordPath);
    bool recordFirst = recordPath != nullptr;
    int hintColumns = 80, hintRows = 24; // size of a new pane until the layout says
    // The first pane is made before SDL and its threads exist (fork); later ones
    // are made on demand, see docs/TABS_PLAN.md.
    Space workspace([&]() -> std::unique_ptr<NativePane> {
        std::function<void(std::string_view)> record;
        if (recordFirst) {
            recordFirst = false;
            record = [&recording](std::string_view bytes) {
                recording.append(bytes);
            };
        }
        auto pane = std::make_unique<NativePane>(
            arguments, term, hintColumns, hintRows, std::move(record));
        pane->v.glide() = CursorGlide([&] {
            CursorGlide::Config c;
            c.enabled = smoothCursor;
            return c;
        }());
        return pane;
    });
    workspace.apply(Command::NewTab);
    SdlRuntime runtime;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("matterm",
                         SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED,
                         960,
                         640,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                             (tabsSmoke ? SDL_WINDOW_HIDDEN : 0)),
        SDL_DestroyWindow);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SdlBackend backend(*window, font, fontSize, tabsSmoke);
    backend.setReserveScrollbarStrip(true);

    ViewConfig viewConfig;
    viewConfig.scrollbarStrip = SdlBackend::scrollbarStrip();
    // Ctrl+B is readline's backward-char; the native app has the desktop
    // shortcuts instead (the browser uses the prefix keys).
    viewConfig.prefixKeys = false;
    WorkspaceView<NativePane> wv(workspace, viewConfig);
    const CursorPtr arrowCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    const CursorPtr columnsCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE));
    const CursorPtr rowsCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS));
    using Cursor = MouseResult::Cursor;
    Cursor shownCursor = Cursor::Default;

    // Window coordinates (events, mouse state) to drawable pixels.
    const auto drawableSize = [&] {
        int w = 0, h = 0;
        if (SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()), &w, &h) <
            0) {
            return std::pair<int, int>{0, 0};
        }
        return std::pair<int, int>{w, h};
    };
    const auto toDrawable = [&](int x, int y) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window.get(), &ww, &wh);
        const auto [dw, dh] = drawableSize();
        if (ww <= 0 || wh <= 0) {
            return std::pair<int, int>{x, y};
        }
        return std::pair<int, int>{int(double(x) * dw / ww),
                                   int(double(y) * dh / wh)};
    };
    const auto updateWindow = [&] {
        const auto [w, h] = drawableSize();
        const WindowMetrics now{
            w, h, backend.cellWidth(), backend.cellHeight()};
        const auto &old = wv.window();
        if (now.width != old.width || now.height != old.height ||
            now.cellWidth != old.cellWidth ||
            now.cellHeight != old.cellHeight) {
            wv.setWindow(now);
        }
        const auto columns = std::max(1, w / std::max(1, now.cellWidth));
        const auto rows = std::max(1, h / std::max(1, now.cellHeight) - 1);
        hintColumns = std::clamp(columns, 1, 65535);
        hintRows = std::clamp(rows, 1, 65535);
    };
    updateWindow();

    SDL_StartTextInput();
    bool running = true;
    bool rejectedInput = false;
    std::string clipboardError;
    std::string linkError;
    std::optional<PtyProcess::ExitStatus> lastExit;
    PaneId selectingPane = 0;
    bool dragging = false;
    using MouseEvent = InputEncoder::MouseEvent;
    bool mouseCaptured = false;
    PaneId capturedPane = 0;
    MouseEvent::Button heldButton = MouseEvent::Button::None;
    std::optional<std::pair<int, int>> lastMouseCell;
    bool skipText = false;
    Uint64 lastFrame = SDL_GetTicks64();

    const auto send = [&](NativePane &pane, std::string_view bytes) {
        if (bytes.empty() || pane.v.ended()) {
            return;
        }
        rejectedInput = !pane.v.sendInput(bytes);
        if (!rejectedInput) {
            pane.v.model().followTail();
        }
    };
    const auto sendFocused = [&](std::string_view bytes) {
        if (auto *pane = workspace.focusedPane()) {
            send(*pane, bytes);
        }
    };
    const auto mouseTracking = [](NativePane &pane) {
        return pane.v.modes().mouseTracking != TerminalModes::MouseTracking::Off;
    };
    const auto sendMouse = [&](NativePane &pane,
                               const PaneMouse &pm,
                               MouseEvent::Type type,
                               MouseEvent::Button button,
                               bool clamp,
                               bool dedupe) {
        const auto cell =
            clamp ? std::optional(pm.clamped) : pm.cell;
        if (!cell || pane.v.ended()) {
            return;
        }
        const auto &model = pane.v.model();
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
        const auto bytes = InputEncoder::mouse(report, pane.v.modes());
        if (!bytes.empty()) {
            rejectedInput = !pane.v.sendInput(bytes);
        }
    };
    const auto copyFrom = [&](NativePane &pane) {
        auto &model = pane.v.model();
        if (pane.v.selection && pane.v.selection->valid(model)) {
            const auto text = pane.v.selection->text(model);
            if (!text) {
                clipboardError = "copy failed: selection exceeds 1 MiB";
            }
            else {
                clipboardError = SDL_SetClipboardText(text->c_str()) == 0
                                     ? ""
                                     : "clipboard copy failed";
            }
        }
    };
    const auto pasteInto = [&](NativePane &pane) {
        std::unique_ptr<char, decltype(&SDL_free)> clipboard(
            SDL_GetClipboardText(), SDL_free);
        if (clipboard && SDL_strlen(clipboard.get()) <= 1024 * 1024) {
            send(pane, InputEncoder::paste(clipboard.get(), pane.v.modes()));
        }
        else {
            rejectedInput = true;
        }
    };
    // Layout results: the program may owe a prefix key; the key may be ours.
    const auto prefixBytes = [&] {
        return InputEncoder::control(
            char(viewConfig.prefix.prefix.key), false);
    };
    const auto afterLayout = [&](const KeyResult &r) {
        if (r.empty) {
            running = false;
        }
        if (r.copy) {
            if (auto *pane = workspace.focusedPane()) {
                copyFrom(*pane);
            }
        }
        if (r.paste) {
            if (auto *pane = workspace.focusedPane()) {
                pasteInto(*pane);
            }
        }
        if (r.forwardPrefix) {
            sendFocused(prefixBytes());
        }
    };
    const auto guarded = [&](auto &&action) {
        try {
            action();
        }
        catch (const std::exception &error) {
            linkError = error.what();
        }
    };

    // --pty-tabs-smoke-test: the same events a user would cause, pushed into the
    // loop, and checks of the workspace after each. Docs/TABS_PLAN.md phase 2.
    int scriptStep = 0;
    Uint64 scriptDue = 0;
    bool scriptDone = false;
    PaneId hiddenPane = 0;
    float hiddenShare = 1;
    int widthBefore = 0;
    PixelRect gutterAtStart;
    const auto expectThat = [](bool ok, const char *what) {
        if (!ok) {
            throw std::runtime_error(std::string("PTY tabs smoke: ") + what);
        }
    };
    const auto pushKey = [](SDL_Keycode sym, Uint16 mods) {
        SDL_SetModState(SDL_Keymod(mods));
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.keysym.sym = sym;
        e.key.keysym.mod = mods;
        SDL_PushEvent(&e);
    };
    const auto pushMouse = [](Uint32 type, int x, int y) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_MOUSEMOTION) {
            e.motion.x = x;
            e.motion.y = y;
        }
        else {
            e.button.x = x;
            e.button.y = y;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.clicks = 1;
        }
        SDL_PushEvent(&e);
    };
    const auto script = [&] {
        if (!tabsSmoke || scriptDone || SDL_GetTicks64() < scriptDue) {
            return;
        }
        SDL_SetModState(KMOD_NONE);
        const auto later = [&](Uint64 ms) { scriptDue = SDL_GetTicks64() + ms; };
        const Uint16 ctrlShift = KMOD_LCTRL | KMOD_LSHIFT;
        switch (scriptStep++) {
        case 0:
            later(500); // the shell starts
            break;
        case 1:
            expectThat(workspace.tabCount() == 1 &&
                           workspace.paneCount() == 1,
                       "one tab with one pane at the start");
            pushKey(SDLK_e, ctrlShift);
            later(300);
            break;
        case 2:
            expectThat(workspace.activeTab()->tree.size() == 2,
                       "Ctrl+Shift+E splits");
            pushKey(SDLK_o, ctrlShift);
            later(300);
            break;
        case 3:
            expectThat(workspace.activeTab()->tree.size() == 3,
                       "Ctrl+Shift+O splits again");
            hiddenPane = workspace.activeTab()->tree.focus();
            pushKey(SDLK_t, ctrlShift);
            later(300);
            break;
        case 4: {
            expectThat(workspace.tabCount() == 2 &&
                           workspace.activeIndex() == 1,
                       "Ctrl+Shift+T opens a tab");
            auto *pane = workspace.pane(hiddenPane);
            expectThat(pane != nullptr, "the hidden pane is kept");
            hiddenShare = pane->v.model().frame(backend.cellHeight()).scrollbar.size;
            later(1500);
            break;
        }
        case 5: {
            auto *pane = workspace.pane(hiddenPane);
            expectThat(pane && !pane->link.finished(),
                       "the hidden process still runs");
            expectThat(pane->v.model().frame(backend.cellHeight()).scrollbar.size <
                           hiddenShare,
                       "the hidden pane keeps receiving output");
            pushKey(SDLK_PAGEUP, KMOD_LCTRL);
            later(300);
            break;
        }
        case 6: {
            expectThat(workspace.activeIndex() == 0, "Ctrl+PgUp goes back");
            const auto plan = wv.plan();
            expectThat(plan.panes.size() == 3 && plan.dividers.size() == 2,
                       "three panes and two dividers");
            gutterAtStart = plan.dividers[0].rect;
            widthBefore = plan.panes[0].rect.w;
            pushMouse(SDL_MOUSEBUTTONDOWN,
                      gutterAtStart.x + gutterAtStart.w * 3 / 4,
                      gutterAtStart.y + 30);
            later(100);
            break;
        }
        case 7:
            pushMouse(SDL_MOUSEMOTION,
                      gutterAtStart.x + gutterAtStart.w * 3 / 4 - 80,
                      gutterAtStart.y + 30);
            later(100);
            break;
        case 8:
            pushMouse(SDL_MOUSEBUTTONUP,
                      gutterAtStart.x + gutterAtStart.w * 3 / 4 - 80,
                      gutterAtStart.y + 30);
            later(300);
            break;
        case 9: {
            const auto plan = wv.plan();
            expectThat(std::abs((widthBefore - plan.panes[0].rect.w) - 80) <=
                           2 * backend.cellWidth(),
                       "dragging the divider moved it");
            for (const auto &pp : plan.panes) {
                auto *pane = workspace.pane(pp.id);
                expectThat(pane && pp.columns >= 20 &&
                               pane->v.model().columns() ==
                                   std::size_t(pp.columns) &&
                               pane->v.model().rows() == std::size_t(pp.rows),
                           "every pane has the planned size");
            }
            pushKey(SDLK_w, ctrlShift);
            later(300);
            break;
        }
        default:
            expectThat(workspace.tabCount() == 1 && workspace.paneCount() == 1,
                       "Ctrl+Shift+W closes the tab and its panes");
            scriptDone = true;
            running = false;
            break;
        }
    };

    while (running) {
        skipText = false;
        updateWindow();
        script();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const std::int64_t now = std::int64_t(SDL_GetTicks64());
            const auto mod = modifiers(SDL_GetModState());
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_TEXTINPUT) {
                if (wv.prefixWaiting()) {
                    // The key after the prefix, as the character it produces.
                    const std::string_view text = event.text.text;
                    const int ch = text.size() == 1 ? int(text[0]) : 0x3fffff;
                    guarded([&] {
                        const auto r =
                            wv.key({ch, false, false, false}, now);
                        afterLayout(r);
                        if (r.forwardKey) {
                            sendFocused(InputEncoder::text(
                                event.text.text, mod.alt));
                        }
                    });
                }
                else if (skipText) {
                    skipText = false;
                }
                else if (!mod.control) {
                    sendFocused(InputEncoder::text(event.text.text, mod.alt));
                }
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN ||
                     event.type == SDL_MOUSEBUTTONUP ||
                     event.type == SDL_MOUSEMOTION) {
                const bool motion = event.type == SDL_MOUSEMOTION;
                const bool down = event.type == SDL_MOUSEBUTTONDOWN;
                const int rawX = motion ? event.motion.x : event.button.x;
                const int rawY = motion ? event.motion.y : event.button.y;
                const auto pos = toDrawable(rawX, rawY);
                const int x = pos.first, y = pos.second;
                MouseInput in;
                in.x = x;
                in.y = y;
                in.shift = mod.shift;
                in.ctrl = mod.control;
                in.alt = mod.alt;
                if (motion) {
                    in.type = MouseInput::Type::Move;
                }
                else {
                    in.type = down ? MouseInput::Type::Down
                                   : MouseInput::Type::Up;
                    in.clicks = event.button.clicks;
                    in.button = event.button.button == SDL_BUTTON_LEFT
                                    ? MouseInput::Button::Left
                                : event.button.button == SDL_BUTTON_MIDDLE
                                    ? MouseInput::Button::Middle
                                : event.button.button == SDL_BUTTON_RIGHT
                                    ? MouseInput::Button::Right
                                    : MouseInput::Button::None;
                }
                MouseResult r;
                guarded([&] { r = wv.mouse(in, now); });
                if (r.empty) {
                    running = false;
                }
                if (r.cursor != shownCursor) {
                    shownCursor = r.cursor;
                    SDL_SetCursor(shownCursor == Cursor::ResizeColumns
                                      ? columnsCursor.get()
                                  : shownCursor == Cursor::ResizeRows
                                      ? rowsCursor.get()
                                      : arrowCursor.get());
                }
                if (!r.pane) {
                    continue;
                }
                const PaneMouse &pm = *r.pane;
                NativePane *target = workspace.pane(pm.pane);
                if (!target) {
                    continue;
                }
                NativePane &np = *target;
                auto &model = np.v.model();
                const auto button = motion || !mouseButton(event.button.button)
                                        ? std::nullopt
                                        : mouseButton(event.button.button);
                if (down && mouseTracking(np) && !mod.shift && button) {
                    mouseCaptured = true;
                    capturedPane = pm.pane;
                    heldButton = *button;
                    lastMouseCell.reset();
                    sendMouse(np, pm, MouseEvent::Type::Press, heldButton,
                              false, false);
                }
                else if (event.type == SDL_MOUSEBUTTONUP && mouseCaptured &&
                         pm.pane == capturedPane && button) {
                    sendMouse(np, pm, MouseEvent::Type::Release, *button, true,
                              false);
                    if (*button == heldButton) {
                        mouseCaptured = false;
                        heldButton = MouseEvent::Button::None;
                    }
                }
                else if (motion && !dragging && mouseTracking(np) &&
                         (mouseCaptured ||
                          (np.v.modes().mouseTracking ==
                               TerminalModes::MouseTracking::Any &&
                           !mod.shift))) {
                    sendMouse(np, pm, MouseEvent::Type::Motion, heldButton,
                              mouseCaptured, true);
                }
                else if (down && in.button == MouseInput::Button::Left) {
                    clipboardError.clear();
                    const auto link =
                        pm.cell && localLinkGesture(mod.control,
                                                    mod.shift,
                                                    mouseTracking(np),
                                                    mouseCaptured)
                            ? urlAt(model, *pm.cell)
                            : std::nullopt;
                    if (link) {
                        dragging = false;
                        linkError = SDL_OpenURL(link->text.c_str()) == 0
                                        ? ""
                                        : std::string("open URL failed: ") +
                                              SDL_GetError();
                    }
                    else {
                        dragging = pm.cell.has_value();
                        selectingPane = pm.pane;
                        if (pm.cell) {
                            selectAt(np.v.selection,
                                     model,
                                     *pm.cell,
                                     mod.shift,
                                     event.button.clicks);
                        }
                        else {
                            np.v.selection.reset();
                        }
                    }
                }
                else if ((motion && dragging) ||
                         (event.type == SDL_MOUSEBUTTONUP &&
                          in.button == MouseInput::Button::Left)) {
                    if (dragging && pm.pane == selectingPane &&
                        np.v.selection && np.v.selection->valid(model)) {
                        np.v.selection->extend(model, pm.clamped);
                    }
                    if (!motion) {
                        dragging = false;
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
                if (auto *pane = workspace.focusedPane()) {
                    send(*pane, InputEncoder::focus(gained, pane->v.modes()));
                }
            }
            else if (event.type == SDL_MOUSEWHEEL) {
                double amount = event.wheel.preciseY != 0 ? event.wheel.preciseY
                                                          : event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    amount = -amount;
                }
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                const auto pos = toDrawable(mx, my);
                const int x = pos.first, y = pos.second;
                WheelResult wr;
                guarded([&] { wr = wv.wheel(x, y, amount); });
                if (wr.consumed || !wr.pane) {
                    continue;
                }
                NativePane *target = workspace.pane(*wr.pane);
                const auto pm = wv.pointerPane(x, y);
                if (!target || !pm) {
                    continue;
                }
                NativePane &np = *target;
                if (mouseTracking(np) && !mod.shift) {
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
                        sendMouse(np, *pm, MouseEvent::Type::Press, button,
                                  true, false);
                    }
                }
                else if (np.v.model().alternateScreen()) {
                    const auto key = amount > 0 ? InputEncoder::Key::Up
                                                : InputEncoder::Key::Down;
                    for (int i = 0;
                         i <
                         int(std::ceil(std::min(10.0, std::abs(amount)) * 3));
                         ++i) {
                        send(np, InputEncoder::key(key, {}, np.v.modes()));
                    }
                }
                else {
                    np.v.model().scrollBy(amount * 3);
                }
            }
            else if (event.type == SDL_KEYDOWN) {
                const auto key = event.key.keysym.sym;
                const bool waiting = wv.prefixWaiting();
                const int code = keyCodeOf(key);
                const bool printable = key >= 32 && key < 127;
                if (mod.control && mod.shift && key == SDLK_q) {
                    running = false;
                    continue;
                }
                if (waiting && printable && !mod.control && !mod.alt) {
                    continue; // the character arrives as text input
                }
                // Layout keys: desktop shortcuts and the prefix machine.
                if (code != 0 && (mod.control || mod.alt || waiting)) {
                    KeyResult r;
                    if (!event.key.repeat || waiting) {
                        guarded([&] {
                            r = wv.key({code, mod.control, mod.shift, mod.alt},
                                       now);
                        });
                    }
                    else {
                        r.forwardKey = true;
                    }
                    afterLayout(r);
                    if (r.changed || r.copy || r.paste) {
                        skipText = true;
                        continue;
                    }
                    if (!r.forwardKey && !r.forwardPrefix) {
                        continue;
                    }
                }
                NativePane *focused = workspace.focusedPane();
                if (!focused) {
                    continue;
                }
                NativePane &np = *focused;
                auto &model = np.v.model();
                if (mod.control && mod.shift && key == SDLK_v) {
                    if (!event.key.repeat) {
                        pasteInto(np);
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_c) {
                    if (!event.key.repeat) {
                        copyFrom(np);
                    }
                }
                else if (mod.control && mod.shift && key == SDLK_a) {
                    if (!event.key.repeat) {
                        np.v.selection = Selection::all(model);
                        dragging = false;
                        clipboardError.clear();
                    }
                }
                else if (mod.shift &&
                         (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)) {
                    model.scrollBy((key == SDLK_PAGEUP ? 1.0 : -1.0) *
                                   model.rows());
                }
                else if (mod.control && mod.shift && key == SDLK_HOME) {
                    model.scrollToOldest();
                }
                else if (mod.control && mod.shift && key == SDLK_END) {
                    model.followTail();
                }
                else if (np.v.modes().applicationKeypad && keypadKey(key)) {
                    skipText = true;
                    send(np, InputEncoder::keypad(*keypadKey(key), mod, np.v.modes()));
                }
                else if (const auto encoded = special(key)) {
                    send(np, InputEncoder::key(*encoded, mod, np.v.modes()));
                }
                else if (mod.control && key >= 0 && key <= 127) {
                    send(np, InputEncoder::control(char(key), mod.alt));
                }
            }
        }
        if (!running) {
            break;
        }
        const std::int64_t nowMs = std::int64_t(SDL_GetTicks64());
        afterLayout(wv.tick(nowMs));
        // Focus in/out reports when the focus moved to another pane.
        const auto moved = wv.takeFocusChange();
        const bool windowFocused =
            (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0;
        if (windowFocused) {
            if (moved.lost != 0) {
                if (auto *p = workspace.pane(moved.lost)) {
                    send(*p, InputEncoder::focus(false, p->v.modes()));
                }
            }
            if (moved.gained != 0 && moved.lost != 0) {
                if (auto *p = workspace.pane(moved.gained)) {
                    send(*p, InputEncoder::focus(true, p->v.modes()));
                }
            }
        }
        guarded([&] { wv.sync(nowMs); });
        // Every process keeps being served, shown or not; ended ones close their pane.
        std::vector<PaneId> ended;
        workspace.forEachPane([&](PaneId id, NativePane &pane, bool) {
            pane.link.pump();
            if (pane.link.finished()) {
                ended.push_back(id);
            }
        });
        for (const PaneId id : ended) {
            if (auto *pane = workspace.pane(id)) {
                lastExit = pane->link.exitStatus();
            }
            if (workspace.closePaneAnywhere(id) == Outcome::Empty) {
                running = false;
            }
        }
        if (!running) {
            break;
        }
        const auto now = SDL_GetTicks64();
        const double seconds = double(now - lastFrame) / 1000;
        lastFrame = now;

        // Draw: strip, panes, gutters.
        const FramePlan plan = wv.plan();
        const Palette stripPalette;
        backend.beginFrame(stripPalette.background);
        if (plan.stripRect.h > 0) {
            CellBuffer buffer(plan.strip.cells.size(), 1);
            for (std::size_t i = 0; i < plan.strip.cells.size(); ++i) {
                buffer.at(i, 0) = plan.strip.cells[i];
            }
            RenderFrame frame;
            frame.cells = buffer.view();
            frame.cursor.visible = false;
            frame.palette = &stripPalette;
            backend.render(frame,
                           PaneArea{{plan.stripRect.x,
                                     plan.stripRect.y,
                                     plan.stripRect.w,
                                     plan.stripRect.h},
                                    {}});
        }
        int pointerX = 0, pointerY = 0;
        {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            std::tie(pointerX, pointerY) = toDrawable(mx, my);
        }
        const auto mouseMod = modifiers(SDL_GetModState());
        std::optional<UrlMatch> shownLink;
        for (const auto &pp : plan.panes) {
            NativePane *pane = workspace.pane(pp.id);
            if (!pane) {
                continue;
            }
            auto &model = pane->v.model();
            model.advanceViewport(seconds);
            auto frame = model.frame(backend.cellHeight());
            frame.palette = &pane->v.palette();
            if (pane->v.selection) {
                if (pane->v.selection->valid(model)) {
                    pane->v.selection->apply(model, frame);
                }
                else {
                    pane->v.selection.reset();
                    if (selectingPane == pp.id) {
                        dragging = false;
                    }
                }
            }
            if (windowFocused && Layout::contains(pp.rect, pointerX, pointerY) &&
                localLinkGesture(mouseMod.control,
                                 mouseMod.shift,
                                 mouseTracking(*pane),
                                 mouseCaptured)) {
                if (const auto pm = wv.pointerPane(pointerX, pointerY);
                    pm && pm->cell) {
                    if (const auto link = urlAt(model, *pm->cell)) {
                        link->underline(model, frame);
                        shownLink = link;
                    }
                }
            }
            frame.textBlinkVisible = (now / 500) % 2 == 0;
            auto &glide = pane->v.glide();
            glide.update(frame.cursor.column,
                         frame.cursor.row,
                         frame.cursor.visible,
                         double(now) / 1000);
            glide.apply(frame.cursor);
            frame.cursor.focused = windowFocused && pp.focused;
            frame.cursor.visible = frame.cursor.visible &&
                                   (!frame.cursor.focused ||
                                    !frame.cursor.blink ||
                                    (now / 500) % 2 == 0);
            backend.render(frame,
                           PaneArea{{pp.rect.x, pp.rect.y, pp.rect.w, pp.rect.h},
                                    {pp.scrollbar.x,
                                     pp.scrollbar.y,
                                     pp.scrollbar.w,
                                     pp.scrollbar.h}});
        }
        for (const auto &d : plan.dividers) {
            backend.drawRect({d.rect.x, d.rect.y, d.rect.w, d.rect.h},
                             Rgb8{46, 51, 60});
        }
        std::string title = "matterm";
        if (const auto *focused = workspace.focusedPane();
            focused && !focused->v.title().empty()) {
            title += " | " + focused->v.title();
        }
        if (rejectedInput) {
            title += " | input queue full/closed";
        }
        if (!clipboardError.empty()) {
            title += " | " + clipboardError;
        }
        if (!linkError.empty()) {
            title += " | " + linkError;
        }
        if (shownLink) {
            title += " | " + shownLink->text;
        }
        SDL_SetWindowTitle(window.get(), title.c_str());
        backend.present();
    }
    SDL_StopTextInput();
    if (tabsSmoke) {
        if (!scriptDone) {
            throw std::runtime_error("PTY tabs smoke: the window closed early");
        }
        std::cout << "PTY tabs smoke test passed\n";
        return 0;
    }
    return lastExit && lastExit->signal == 0 ? std::max(0, lastExit->code) : 0;
}
