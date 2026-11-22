#include "ptyfrontend.h"

#include "cellbuffer.h"
#include "cursorglide.h"
#include "inputencoder.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"
#include "ptylink.h"
#include "procinfo.h"
#include "sessionfiles.h"
#include "sessionrestore.h"
#include "terminalview.h"
#include "ui/commandpalette.h"
#include "ui/dialogs.h"
#include "workspaceview.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
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

// SIGHUP (the terminal that started us went away) and SIGTERM: a controlled shutdown that saves
// a saved session first. Only sets a flag; the main loop does the work.
volatile std::sig_atomic_t gShutdownSignal = 0;
void onShutdownSignal(int) {
    gShutdownSignal = 1;
}
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
    // Folder the next pane's shell should start in (a restored session), else empty.
    std::string nextCwd;
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
        std::vector<std::string> command = arguments;
        if (!nextCwd.empty()) {
            // cd, then become the usual command (the pane's process stays the shell, so /proc
            // and the close question see the shell). A folder that vanished was checked before.
            command = {"/bin/sh", "-c", "cd -- \"$1\" 2>/dev/null; shift; exec \"$@\"", "sh", nextCwd};
            command.insert(command.end(), arguments.begin(), arguments.end());
        }
        auto pane = std::make_unique<NativePane>(
            command, term, hintColumns, hintRows, std::move(record));
        pane->v.glide() = CursorGlide([&] {
            CursorGlide::Config c;
            c.enabled = smoothCursor;
            return c;
        }());
        return pane;
    });
    workspace.apply(Command::NewTab);
    SdlRuntime runtime;
    {
        // After SDL_Init, which installs its own SIGTERM handler (a plain quit): ours saves first.
        struct sigaction action {};
        action.sa_handler = onShutdownSignal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
    }
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
    viewConfig.paletteButton = true;
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
    std::function<void(const KeyResult &)> keyExtras; // palette, dialogs, actions (set below)
    const auto afterLayout = [&](const KeyResult &r) {
        if (r.empty) {
            running = false;
        }
        if (keyExtras) {
            keyExtras(r);
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

    // ---- palette, dialogs, saved sessions (docs/TABS_PLAN.md sections 8 and 9) ----
    LinuxProcSource procSource;
    const SessionFiles sessionFiles(SessionFiles::defaultDirectory());
    SessionPolicy sessionPolicy;
    bool smokeBusy = false; // --pty-tabs-smoke-test: pretend every pane runs a program
    const auto paneState = [&](NativePane &pane) {
        return probePane(procSource, pane.link.childPid(), pane.link.foregroundProcessGroup());
    };
    {
        // Closing a pane, tab or the window asks when a program other than the shell runs there.
        wv.setBusyProbe([&](PaneId id) -> std::optional<std::string> {
            NativePane *pane = workspace.pane(id);
            if (!pane || pane->link.finished()) {
                return std::nullopt;
            }
            if (smokeBusy) {
                return std::string("smoke program");
            }
            const auto state = paneState(*pane);
            if (!state.busy) {
                return std::nullopt;
            }
            return commandLine(state.command); // Empty: the dialog says "A program".
        });
    }
    const auto showCloseDialog = [&](const CloseRequest &request) {
        std::vector<ui::ConfirmDialog::Button> buttons = {{"Close", "close.confirm"}, {"Cancel", "close.cancel"}};
        // Hook: when request.backgroundAvailable, a third button "Run in background" goes here.
        wv.pushLayer(std::make_unique<ui::ConfirmDialog>(request.question(), std::move(buttons), 1, 1));
    };
    const auto requestQuit = [&] {
        if (auto request = wv.windowClose()) {
            showCloseDialog(*request);
        }
        else {
            running = false;
        }
    };
    const auto notify = [&](const std::string &text) {
        linkError = text; // Shown in the window title, like the other errors of the app.
    };
    // The window's session as it is now. Screen text only for a shutdown checkpoint.
    const auto saveSession = [&](const std::string &name, bool withText) {
        const auto snapshot = captureSession(
            workspace, name, std::int64_t(std::time(nullptr)), [&](PaneId, const NativePane &pane) {
                SavedPane saved;
                const auto state = paneState(const_cast<NativePane &>(pane));
                saved.cwd = state.cwd;
                saved.command = state.command;
                if (withText) {
                    saved.text = visibleText(pane.v.model());
                }
                return saved;
            });
        if (!snapshot) {
            return false;
        }
        std::string error;
        if (!sessionFiles.save(*snapshot, &error)) {
            notify("session not saved: " + error);
            return false;
        }
        return true;
    };
    const auto restoreByName = [&](const std::string &name) {
        const auto snapshot = sessionFiles.load(name);
        if (!snapshot) {
            notify("session '" + name + "' cannot be read");
            return;
        }
        std::vector<std::string> gone; // Folders that no longer exist, by creation order.
        const auto report = restoreSession(workspace, *snapshot, [&](const SavedPane &saved) {
            std::error_code ec;
            nextCwd = !saved.cwd.empty() && std::filesystem::is_directory(saved.cwd, ec) ? saved.cwd : std::string();
        });
        nextCwd.clear();
        for (const auto &restored : report.panes) {
            NativePane *pane = workspace.pane(restored.id);
            if (!pane || !restored.saved) {
                continue;
            }
            const SavedPane &saved = *restored.saved;
            std::error_code ec;
            if (!saved.cwd.empty() && !std::filesystem::is_directory(saved.cwd, ec)) {
                pane->v.note("restored without its folder (gone): " + CloseRequest::shown(saved.cwd));
            }
            if (!saved.text.empty()) {
                std::string crlf;
                for (const char c : saved.text) {
                    crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
                }
                pane->v.note("previous screen:\r\n" + crlf);
            }
            if (const auto offer = offerText(saved); !offer.empty()) {
                pane->v.note(offer);
                // On the prompt line, without a newline: the owner presses Enter, edits or ignores it.
                send(*pane, rerunInput(saved));
            }
        }
        if (report.refused > 0) {
            notify("session partly restored: no room for everything");
        }
        sessionPolicy.adopt(name);
    };
    const auto openPalette = [&] {
        std::vector<CommandEntry> extra = {
            {"session.open", "Open session...", "Choose a saved session to open", std::nullopt, {}, {}},
            {"session.save", "Save session as...", "Save the tabs and panes under a name", std::nullopt, {}, {}},
            {"tab.rename", "Rename tab...", "Give the tab a name that stays", std::nullopt, {}, {}},
        };
        wv.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry(viewConfig.prefix, extra), true));
    };
    const auto handleAction = [&](const std::string &action) {
        const auto colon = action.find(':');
        const std::string id = action.substr(0, colon);
        const std::string argument = colon == std::string::npos ? std::string() : action.substr(colon + 1);
        if (id == "close.confirm" || id == "close.cancel") {
            const auto pending = wv.pendingClose();
            const auto result = wv.answerClose(id == "close.confirm" ? CloseAnswer::Close : CloseAnswer::Cancel);
            if (id == "close.confirm") {
                if (pending && pending->scope == CloseScope::Window) {
                    running = false;
                }
                else if (result.empty) {
                    running = false;
                }
            }
        }
        else if (id == "session.open" && colon == std::string::npos) {
            std::vector<ui::PickList::Item> items;
            for (const auto &info : sessionFiles.list()) {
                items.push_back({info.name,
                                 std::to_string(info.tabs) + (info.tabs == 1 ? " tab, " : " tabs, ") +
                                     std::to_string(info.panes) + (info.panes == 1 ? " pane" : " panes"),
                                 "session.open:" + info.name});
            }
            wv.pushLayer(std::make_unique<ui::PickList>("Open session", std::move(items), "No saved sessions"));
        }
        else if (id == "session.open") {
            restoreByName(argument);
        }
        else if (id == "session.save" && colon == std::string::npos) {
            wv.pushLayer(std::make_unique<ui::TextPrompt>("Save session as", sessionPolicy.name(), "session.save:", false));
        }
        else if (id == "session.save") {
            if (!SessionSnapshot::validName(argument)) {
                notify("session names use letters, digits, '.', '_' and '-'");
            }
            else {
                sessionPolicy.adopt(argument);
                saveSession(argument, false);
            }
        }
        else if (id == "tab.rename" && colon == std::string::npos) {
            wv.pushLayer(std::make_unique<ui::TextPrompt>("Rename tab", workspace.userTitle(workspace.activeIndex()), "tab.rename:", true));
        }
        else if (id == "tab.rename") {
            workspace.setUserTitle(workspace.activeIndex(), SessionSnapshot::cleanTitle(argument));
            if (sessionPolicy.shouldSave(SessionPolicy::Event::TabNamed)) {
                saveSession(sessionPolicy.name(), false);
            }
        }
    };
    // What the layout says besides its own changes: a palette to open, an answer from an overlay,
    // a question before closing.
    const auto layoutExtras = [&](const auto &r) {
        if (r.openPalette) {
            openPalette();
        }
        if (r.confirm) {
            showCloseDialog(*r.confirm);
        }
        if (!r.action.empty()) {
            handleAction(r.action);
        }
    };

    keyExtras = [&](const KeyResult &r) { layoutExtras(r); };

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
        case 10:
            expectThat(workspace.tabCount() == 1 && workspace.paneCount() == 1,
                       "Ctrl+Shift+W closes the tab and its panes");
            pushKey(SDLK_p, ctrlShift);
            later(300);
            break;
        case 11:
            expectThat(wv.hasLayer(), "Ctrl+Shift+P opens the palette");
            pushKey(SDLK_ESCAPE, 0);
            later(300);
            break;
        case 12: {
            expectThat(!wv.hasLayer(), "Escape closes the palette");
            // Save and open a session through the same actions the palette runs.
            handleAction("tab.rename:smoke tab");
            expectThat(workspace.userTitle(0) == "smoke tab", "renaming a tab sets its title");
            handleAction("session.save:smoke");
            bool saved = false;
            for (const auto &info : sessionFiles.list()) {
                saved = saved || (info.name == "smoke" && info.tabs == 1 && info.panes == 1);
            }
            expectThat(saved, "the session was saved");
            handleAction("session.open:smoke");
            expectThat(workspace.tabCount() == 2 && workspace.paneCount() == 2,
                       "opening the session adds its tab");
            expectThat(workspace.userTitle(1) == "smoke tab",
                       "the restored tab has its title");
            handleAction("session.open");
            expectThat(wv.hasLayer(), "the session list opens");
            pushKey(SDLK_ESCAPE, 0);
            later(300);
            break;
        }
        case 13:
            expectThat(!wv.hasLayer(), "Escape closes the session list");
            smokeBusy = true;
            pushKey(SDLK_w, ctrlShift);
            later(300);
            break;
        case 14:
            expectThat(wv.hasLayer() && wv.pendingClose() && workspace.tabCount() == 2,
                       "closing a tab that runs a program asks first");
            pushKey(SDLK_ESCAPE, 0);
            later(300);
            break;
        case 15:
            expectThat(!wv.hasLayer() && !wv.pendingClose() && workspace.tabCount() == 2,
                       "Escape cancels the close");
            pushKey(SDLK_w, ctrlShift);
            later(300);
            break;
        case 16:
            expectThat(wv.hasLayer(), "the question is asked again");
            pushKey(SDLK_LEFT, 0); // from Cancel to Close
            later(200);
            break;
        case 17:
            pushKey(SDLK_RETURN, 0);
            later(300);
            break;
        case 18: {
            expectThat(!wv.hasLayer() && workspace.tabCount() == 1, "Close closes the tab");
            SDL_Event quit{};
            quit.type = SDL_QUIT; // The window's close button.
            SDL_PushEvent(&quit);
            later(300);
            break;
        }
        case 19:
            expectThat(wv.hasLayer() && wv.pendingClose() &&
                           wv.pendingClose()->scope == CloseScope::Window,
                       "closing the window asks too");
            pushKey(SDLK_LEFT, 0);
            later(200);
            break;
        case 20:
            scriptDone = true;
            pushKey(SDLK_RETURN, 0); // Close: the window quits
            later(300);
            break;
        default:
            expectThat(!wv.hasLayer() && !wv.pendingClose(),
                       "no overlay is left open");
            scriptDone = true;
            running = false;
            break;
        }
    };

    while (running) {
        skipText = false;
        if (gShutdownSignal != 0) {
            break; // SIGTERM or SIGHUP: leave through the shutdown save below, no question asked.
        }
        if (wv.pendingClose() && !wv.hasLayer()) {
            wv.answerClose(CloseAnswer::Cancel); // The dialog was dismissed by a click outside.
        }
        updateWindow();
        script();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const std::int64_t now = std::int64_t(SDL_GetTicks64());
            const auto mod = modifiers(SDL_GetModState());
            if (event.type == SDL_QUIT) {
                requestQuit();
            }
            else if (event.type == SDL_TEXTINPUT && wv.hasLayer()) {
                // An overlay is open: typed characters go to it, one chord per code point.
                const std::string_view text = event.text.text;
                for (std::size_t i = 0; i < text.size();) {
                    const auto lead = static_cast<unsigned char>(text[i]);
                    const std::size_t n = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                    if (i + n > text.size()) {
                        break;
                    }
                    char32_t cp = n == 1 ? lead : char32_t(lead & (0xff >> (n + 1)));
                    for (std::size_t k = 1; k < n; ++k) {
                        cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
                    }
                    i += n;
                    guarded([&] {
                        const auto r = wv.key({int(cp), false, false, false}, now);
                        afterLayout(r);
                    });
                }
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
                layoutExtras(r);
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
                    requestQuit();
                    continue;
                }
                if (wv.hasLayer()) {
                    // An overlay is open: named keys and Ctrl chords go to it; plain printable
                    // keys arrive as text input.
                    if (code != 0 && (!printable || mod.control || mod.alt)) {
                        guarded([&] {
                            const auto r = wv.key({code, mod.control, mod.shift, mod.alt}, now);
                            afterLayout(r);
                        });
                    }
                    continue;
                }
                if (waiting && printable && !mod.control && !mod.alt) {
                    continue; // the character arrives as text input
                }
                // Layout keys: desktop shortcuts and the prefix machine.
                if (code != 0 && (mod.control || mod.alt || waiting)) {
                    KeyResult r;
                    guarded([&] {
                        r = wv.key({code, mod.control, mod.shift, mod.alt}, now,
                                   event.key.repeat != 0);
                    });
                    afterLayout(r);
                    if (r.changed || r.copy || r.paste || r.openPalette || r.confirm) {
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
        for (const auto &layer : plan.layers) {
            RenderFrame frame;
            frame.cells = layer.cells->view();
            frame.cursor = layer.cursor;
            frame.cursor.focused = true;
            frame.palette = &stripPalette;
            backend.render(frame,
                           PaneArea{{layer.rect.x, layer.rect.y, layer.rect.w, layer.rect.h}, {}});
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
    // A controlled shutdown (quit, the window's close button, SIGTERM, SIGHUP) writes the session
    // of a window the owner made persistent, with a checkpoint of what is on the screens. Nothing
    // is written for a window whose session was never saved, and nothing is ever restored on start.
    if (sessionPolicy.shouldSave(SessionPolicy::Event::Shutdown)) {
        saveSession(sessionPolicy.name(), sessionPolicy.withScreenText(SessionPolicy::Event::Shutdown));
    }
    if (tabsSmoke) {
        if (!scriptDone) {
            throw std::runtime_error("PTY tabs smoke: the window closed early");
        }
        std::cout << "PTY tabs smoke test passed\n";
        return 0;
    }
    return lastExit && lastExit->signal == 0 ? std::max(0, lastExit->code) : 0;
}
