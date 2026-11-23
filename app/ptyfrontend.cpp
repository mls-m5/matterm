#include "ptyfrontend.h"
#include "daemonconnect.h"

#include "borderless.h"

#include "cellbuffer.h"
#include "cursorglide.h"
#include "daemoncommands.h"
#include "sessioncommands.h"
#include "daemonclient.h"
#include "daemonlink.h"
#include "inputencoder.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalmodel.h"
#include "ptylink.h"
#include "procinfo.h"
#include "sessionfiles.h"
#include "framesignature.h"
#include "sessionrestore.h"
#include "sshtunnel.h"
#include "terminalview.h"
#include "ui/commandpalette.h"
#include "ui/dialogs.h"
#include "workspaceview.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <deque>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
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

// --snap-scroll: scrolling comes to rest on whole rows (default: pixel scrolling).
bool gSnapScroll = false;

TerminalViewConfig paneViewConfig(int columns, int rows) {
    TerminalViewConfig config{std::size_t(columns), std::size_t(rows), 1000, {}};
    config.snapScrollToRows = gSnapScroll;
    return config;
}

// A daemon pane says plainly what happened when another window took its terminal over.
TerminalViewConfig daemonViewConfig(int columns, int rows) {
    TerminalViewConfig config = paneViewConfig(columns, rows);
    config.replacedNotice = "This terminal is controlled from another window. Press Enter to take it back";
    return config;
}

// Time for the daemon connection's timers.
protocol::Time daemonClock() {
    return std::chrono::duration_cast<protocol::Time>(std::chrono::steady_clock::now().time_since_epoch());
}

// One terminal of the window: a process (here, or in the daemon) and the view of its output.
// The links are declared first because the view borrows them.
struct NativePane {
    std::unique_ptr<PtyLink> link;      // The program runs in this process, or
    std::unique_ptr<DaemonLink> daemon; // in matterm-daemon, which keeps it when this window goes.
    std::string socket;                 // The daemon's socket (daemon panes).
    std::unique_ptr<TapLink> tap;
    TerminalView v;
    bool endInDaemon = true; // Closing the pane ends its terminal; false: leave it running.
    bool hinted = false;
    bool shared = false;           // Marked shared in the daemon (offered to browsers).
    bool takeoverReported = false; // The owner was told that another window took this terminal.
    std::string name;              // The daemon's name for the terminal (Rename terminal...), if any.

    NativePane(const std::vector<std::string> &arguments,
               const std::string &term,
               int columns,
               int rows,
               std::function<void(std::string_view)> record)
        : link(std::make_unique<PtyLink>(
              arguments, columns, rows, term, PtyProcess::ColorCapability::TrueColor))
        , v(paneViewConfig(columns, rows)) {
        if (record) {
            tap = std::make_unique<TapLink>(*link, std::move(record));
            v.attach(*tap);
        }
        else {
            v.attach(*link);
        }
    }
    // A pane whose terminal lives in the daemon: a new one (empty `sessionId`) or an existing one.
    // A new terminal starts in `cwd` (empty: the daemon's folder).
    NativePane(const std::string &socketPath, std::string sessionId, const std::string &cwd, int columns, int rows)
        : socket(socketPath)
        , v(daemonViewConfig(columns, rows)) {
        if (sessionId.empty()) {
            std::string error;
            const auto reply = askDaemon(
                socket, mtd::CreateSession{cwd, std::uint16_t(columns), std::uint16_t(rows)}, error);
            if (const auto *created = reply ? std::get_if<mtd::SessionCreated>(&*reply) : nullptr) {
                sessionId = created->id;
            }
            // Otherwise attach without an id: the daemon makes the terminal itself (its own folder).
        }
        daemon = attachToDaemon(v, socket, std::move(sessionId), 0, daemonClock);
    }
    ~NativePane() {
        v.detach();
        if (daemon && endInDaemon && !v.ended()) {
            daemon->close();
        }
    }
    NativePane(const NativePane &) = delete;
    NativePane &operator=(const NativePane &) = delete;

    TerminalView &view() {
        return v;
    }
    const TerminalView &view() const {
        return v;
    }
    bool inDaemon() const {
        return !link;
    }
    // Shows another daemon terminal in this pane. The old one is ended when `endOld`, else it
    // keeps running in the daemon, detached.
    void swapTerminal(const std::string &sessionId, bool endOld) {
        v.detach();
        if (daemon && endOld && !v.ended()) {
            daemon->close();
        }
        daemon.reset();
        v.resetForReplay();
        daemon = attachToDaemon(v, socket, sessionId, 0, daemonClock);
        endInDaemon = true;
        shared = false;
        name.clear();
        hinted = false;
        takeoverReported = false;
    }
    // Serves the process or the connection; says what Enter will do when the terminal is not live.
    void pump() {
        if (link) {
            link->pump();
            return;
        }
        if (daemon) {
            daemon->tick();
        }
        const auto kind = reattachKind(v);
        if (kind == Reattach::None) {
            hinted = false;
        }
        else if (!hinted) {
            hinted = true;
            if (kind == Reattach::Same && v.state() == TerminalView::State::Replaced) {
                return; // The notice already says it.
            }
            v.note(kind == Reattach::Fresh ? "press Enter to start a new terminal"
                                           : "press Enter to attach again");
        }
    }
    bool finished() const {
        return link ? link->finished() : v.state() == TerminalView::State::Exited;
    }
    std::optional<PtyProcess::ExitStatus> exitStatus() const {
        if (link) {
            return link->exitStatus();
        }
        if (v.state() == TerminalView::State::Exited) {
            return PtyProcess::ExitStatus{v.exitCode(), v.exitSignal()};
        }
        return std::nullopt;
    }
    // Enter in a pane that lost its terminal. True when it did something.
    bool reattach() {
        if (link || reattachKind(v) == Reattach::None) {
            return false;
        }
        auto fresh = reattachToDaemon(v, daemon.get(), socket, daemonClock);
        if (fresh) {
            daemon = std::move(fresh);
            hinted = false;
        }
        return true;
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
    case SDLK_HOME:
        return keys::Home;
    case SDLK_END:
        return keys::End;
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
                   bool tabsSmoke,
                   const DaemonOptions &daemonOptionsIn) {
    DaemonOptions daemonOptions = daemonOptionsIn;
    gSnapScroll = daemonOptions.snapScroll;
    if (smokeTest) {
        return runSmokeTest(font, fontSize, arguments, term);
    }
    // The daemon is found (or started) before SDL and its threads exist.
    std::unique_ptr<frontend::SshTunnel> tunnel; // Lives as long as the window (the socket is its).
    if (!daemonOptions.host.empty()) {
        tunnel = openDaemonTunnel(daemonOptions);
    }
    const bool remoteDaemon = tunnel != nullptr;
    // Notices that ssh ended and starts it again on a worker thread (declared
    // after the tunnel: it goes first).
    std::unique_ptr<frontend::TunnelReconnector> reconnector;
    if (tunnel) {
        reconnector = std::make_unique<frontend::TunnelReconnector>(*tunnel);
    }
    const std::string daemonPath = daemonSocket(daemonOptions.socketPath);
    std::optional<std::string> daemonFallbackReason; // set: the daemon was wanted and is not there
    if (daemonOptions.enabled) {
        const auto error = ensureDaemon(daemonOptions.socketPath);
        if (!error.empty()) {
            if (daemonOptions.required) {
                throw std::runtime_error("--daemon: " + error);
            }
            std::cerr << "matterm: no daemon (" << error << "); running the programs here\n";
            daemonOptions.enabled = false;
            daemonFallbackReason = error;
        }
    }
    Uint64 lastSharedPoll = 0;
    bool keepTerminals = false;           // Leave the programs running in the daemon when we go.
    std::uint32_t daemonStopCount = 0;    // What the "Stop daemon" question named.
    OutputRecording recording(recordPath);
    bool recordFirst = recordPath != nullptr;
    int hintColumns = 80, hintRows = 24; // size of a new pane until the layout says
    // Folder the next pane's shell should start in (a restored session), else empty.
    std::string nextCwd;
    // A daemon terminal the next pane should show instead of making a new one (Attach terminal).
    std::string nextAttachId;
    // Where a new daemon terminal starts: the saved folder, else the folder of the pane in focus
    // (as the daemon sees it), else this program's folder.
    // Replaced below once the probe exists; the first pane is made before that.
    std::function<std::string()> daemonCwd = [remoteDaemon] {
        if (remoteDaemon) {
            return std::string(); // A folder of this computer means nothing there.
        }
        std::error_code ec;
        const auto here = std::filesystem::current_path(ec);
        return ec ? std::string() : here.string();
    };
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
        auto pane = daemonOptions.enabled
                        ? std::make_unique<NativePane>(daemonPath, nextAttachId, daemonCwd(), hintColumns, hintRows)
                        : std::make_unique<NativePane>(command, term, hintColumns, hintRows, std::move(record));
        pane->v.glide() = CursorGlide([&] {
            CursorGlide::Config c;
            c.enabled = smoothCursor;
            return c;
        }());
        return pane;
    });
    workspace.apply(Command::NewTab);
    if (daemonFallbackReason) {
        // Said in the first terminal, where it is seen (the window title and stderr are not enough).
        if (NativePane *first = workspace.focusedPane()) {
            first->v.model().write(daemonFallbackNote(*daemonFallbackReason));
        }
    }
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
                             (daemonOptions.borderless ? SDL_WINDOW_BORDERLESS : 0) |
                             (tabsSmoke ? SDL_WINDOW_HIDDEN : 0)),
        SDL_DestroyWindow);
    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }
    SdlBackend backend(*window, font, fontSize, tabsSmoke);
    backend.setReserveScrollbarStrip(true);

    // --borderless (docs/TABS_PLAN.md section 16): the strip is the frame, if the hit test works;
    // otherwise the system frame comes back, and the window says so. No half modes.
    borderless::HitState hitState;
    bool ownFrame = false;
    if (daemonOptions.borderless) {
        // Edge resizing through the hit test needs a resizable window (Wayland ignores it otherwise).
        SDL_SetWindowResizable(window.get(), SDL_TRUE);
        // MATTERM_ASSUME_HIT_TEST=1 (self-tests under the dummy video driver, which has no hit test)
        // takes the own frame as if the hit test had worked, so the border and inset code runs.
        const char *assume = std::getenv("MATTERM_ASSUME_HIT_TEST");
        const bool installed = borderless::install(window.get(), hitState) == 0 || (assume && *assume == '1');
        const frontend::FrameChoice frame =
            frontend::chooseFrame(true, installed, installed ? std::string() : std::string(SDL_GetError()));
        ownFrame = frame.mode == frontend::FrameMode::Own;
        if (!ownFrame) {
            SDL_SetWindowBordered(window.get(), SDL_TRUE);
            std::cerr << frame.note << '\n';
            if (NativePane *first = workspace.focusedPane()) {
                std::u32string text;
                for (const char c : frame.note + "\r\n") {
                    text += static_cast<char32_t>(static_cast<unsigned char>(c));
                }
                first->v.model().write(text);
            }
        }
    }

    ViewConfig viewConfig;
    viewConfig.windowButtons = ownFrame;
    viewConfig.scrollbarStrip = SdlBackend::scrollbarStrip();
    // Ctrl+B is readline's backward-char; the native app has the desktop
    // shortcuts instead (the browser uses the prefix keys).
    viewConfig.prefixKeys = false;
    parseTabActivity(daemonOptions.tabActivity, viewConfig.tabActivity);
    viewConfig.paletteButton = true;
    viewConfig.shareButton = daemonOptions.enabled;
    WorkspaceView<NativePane> wv(workspace, viewConfig);
    const CursorPtr arrowCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    const CursorPtr columnsCursor(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE));
    const CursorPtr rowsCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS));
    using Cursor = MouseResult::Cursor;
    Cursor shownCursor = Cursor::Default;
    // The pointer over the edges of a borderless window: system cursors, made once. Where the
    // desktop has no cursor theme SDL may give none; then the pointer simply stays an arrow.
    CursorPtr edgeCursors[4];
    frontend::CursorShape edgeShape = frontend::CursorShape::Default;
    if (ownFrame) {
        const SDL_SystemCursor ids[4] = {SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS,
                                         SDL_SYSTEM_CURSOR_SIZENWSE, SDL_SYSTEM_CURSOR_SIZENESW};
        for (int i = 0; i < 4; ++i) {
            edgeCursors[i].reset(SDL_CreateSystemCursor(ids[i]));
            if (!edgeCursors[i]) {
                std::cerr << "matterm: no resize pointer shape from the desktop (" << SDL_GetError()
                          << "); the pointer stays an arrow at the window edges\n";
                break;
            }
        }
    }
    const auto appCursor = [&]() -> SDL_Cursor * {
        return shownCursor == Cursor::ResizeColumns ? columnsCursor.get()
               : shownCursor == Cursor::ResizeRows  ? rowsCursor.get()
                                                    : arrowCursor.get();
    };
    // Same classification as the hit test, so what shows a resize pointer is what resizes.
    const auto showEdgeCursor = [&](frontend::CursorShape shape) {
        if (shape == edgeShape) {
            return;
        }
        edgeShape = shape;
        if (shape == frontend::CursorShape::Default) {
            SDL_SetCursor(appCursor()); // Back to what the program uses otherwise.
        }
        else if (SDL_Cursor *cursor = edgeCursors[int(shape) - 1].get()) {
            SDL_SetCursor(cursor);
        }
    };

    // Window coordinates (events, mouse state) to drawable pixels.
    const auto drawableSize = [&] {
        int w = 0, h = 0;
        if (SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()), &w, &h) <
            0) {
            return std::pair<int, int>{0, 0};
        }
        return std::pair<int, int>{w, h};
    };
    // The border of --borderless is outside the view: the strip, panes and layers are laid out
    // inside it, so it never covers content (left, right and bottom only; none when maximised).
    const auto insetNow = [&] {
        return frontend::windowInset(ownFrame, (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) != 0);
    };
    // The view's own pixels: coordinates relative to its top left corner, inside the border.
    const auto toDrawable = [&](int x, int y) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window.get(), &ww, &wh);
        const auto [dw, dh] = drawableSize();
        const frontend::WindowInset inset = insetNow();
        if (ww <= 0 || wh <= 0) {
            return std::pair<int, int>{x - inset.left, y - inset.top};
        }
        return std::pair<int, int>{int(double(x) * dw / ww) - inset.left,
                                   int(double(y) * dh / wh) - inset.top};
    };
    const auto updateWindow = [&] {
        const auto [fullW, fullH] = drawableSize();
        const frontend::WindowInset inset = insetNow();
        const int w = std::max(1, fullW - inset.left - inset.right);
        const int h = std::max(1, fullH - inset.top - inset.bottom);
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
        if (bytes.empty() || pane.v.ended() || pane.v.copyMode) {
            return; // Copy mode: the program gets no input.
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
    // Copy / scroll mode (docs/TABS_PLAN.md section 12).
    const auto enterCopyMode = [&] {
        if (auto *pane = workspace.focusedPane(); pane && !pane->v.ended() && !pane->v.copyMode) {
            pane->v.selection.reset();
            pane->v.copyMode.emplace(pane->v.model());
        }
    };
    const auto copyKey = [&](NativePane &pane, const KeyChord &chord) {
        const auto r = pane.v.copyMode->key(chord, pane.v.model(), pane.v.selection);
        if (r.action == CopyMode::Action::Copy) {
            clipboardError = SDL_SetClipboardText(r.text.c_str()) == 0 ? "" : "clipboard copy failed";
        }
        else if (r.tooLarge) {
            clipboardError = "copy failed: selection exceeds 1 MiB";
        }
        if (r.action != CopyMode::Action::None) {
            pane.v.copyMode.reset();
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
    const SessionFiles layoutFiles(SessionFiles::defaultLayoutDirectory()); // Detached tabs.
    SessionPolicy sessionPolicy;
    bool smokeBusy = false; // --pty-tabs-smoke-test: pretend every pane runs a program
    const auto paneState = [&](NativePane &pane) {
        if (!pane.link) {
            // The program is in the daemon, which reads /proc for us.
            PaneState state;
            if (!pane.daemon || pane.daemon->sessionId().empty()) {
                return state;
            }
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::GetSessionInfo{pane.daemon->sessionId()}, error, 500);
            if (const auto *info = reply ? std::get_if<mtd::SessionInfo>(&*reply) : nullptr) {
                state.cwd = info->cwd;
                state.busy = info->busy;
                state.command = info->command;
            }
            return state;
        }
        return probePane(procSource, pane.link ? pane.link->childPid() : 0, pane.link ? pane.link->foregroundProcessGroup() : 0);
    };
    daemonCwd = [&]() -> std::string {
        if (remoteDaemon) {
            return {}; // The other computer's shell starts in its own home.
        }
        if (!nextCwd.empty()) {
            return nextCwd;
        }
        if (const NativePane *focused = workspace.focusedPane(); focused && focused->daemon) {
            const auto cwd = paneState(const_cast<NativePane &>(*focused)).cwd;
            if (!cwd.empty()) {
                return cwd;
            }
        }
        std::error_code ec;
        const auto here = std::filesystem::current_path(ec);
        return ec ? std::string() : here.string();
    };
    {
        // Closing a pane, tab or the window asks when a program other than the shell runs there.
        wv.setBusyProbe([&](PaneId id) -> std::optional<std::string> {
            NativePane *pane = workspace.pane(id);
            if (!pane || pane->finished()) {
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
    const auto notify = [&](const std::string &text) {
        linkError = text; // Shown in the window title, like the other errors of the app.
    };
    if (daemonOptions.enabled) {
        wv.setShareProbe([&](PaneId id) {
            const NativePane *pane = workspace.pane(id);
            return pane && pane->shared;
        });
    }
    // Marks the given panes shared (or private) in the daemon, and takes over what the daemon
    // answers: its list says which sessions are shared now.
    const auto setShared = [&](const std::vector<PaneId> &ids, bool on) {
        std::string error;
        std::optional<mtd::ControlReply> last;
        for (const PaneId id : ids) {
            NativePane *pane = workspace.pane(id);
            if (!pane || !pane->daemon || pane->daemon->sessionId().empty()) {
                continue;
            }
            last = askDaemon(daemonPath, mtd::SetShared{pane->daemon->sessionId(), on}, error);
            if (!last) {
                break;
            }
        }
        const auto *list = last ? std::get_if<mtd::SessionList>(&*last) : nullptr;
        if (!list) {
            if (!last && !error.empty()) {
                notify("daemon: " + error);
            }
            return;
        }
        workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
            if (!pane.daemon) {
                return;
            }
            for (const auto &entry : list->sessions) {
                if (entry.id == pane.daemon->sessionId()) {
                    pane.shared = entry.shared;
                    pane.name = entry.name;
                }
            }
        });
        notify(on ? "shared" : "sharing stopped");
    };
    const auto activeTabPanes = [&] {
        std::vector<PaneId> ids;
        workspace.forEachPane([&](PaneId id, NativePane &, bool visible) {
            if (visible) {
                ids.push_back(id);
            }
        });
        return ids;
    };
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
    // The window's session as it is now. Screen text only for a shutdown checkpoint.
    // This window's session: the owner's choice, else the name of the first tab.
    const auto sessionName = [&] {
        return sessionPolicy.suggestion(workspace.tabCount() > 0 ? workspace.userTitle(0) : std::string());
    };
    std::string autoSavedName; // The file this window wrote by itself (not chosen by the owner).
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
    // Writes this window's session under its name (see sessionName). A name the owner did not
    // choose follows the first tab: the file of the old name goes away.
    const auto autoSave = [&](bool withText) {
        const std::string name = sessionName();
        if (!sessionPolicy.named() && !autoSavedName.empty() && autoSavedName != name) {
            sessionFiles.remove(autoSavedName);
            autoSavedName.clear();
        }
        if (saveSession(name, withText) && !sessionPolicy.named()) {
            autoSavedName = name;
        }
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
    // The terminals the daemon holds, with what the list needs (a short answer or none: the
    // daemon is local). `quiet`: no notice when it does not answer.
    const auto daemonSessions = [&](bool quiet = true) {
        std::vector<AttachableSession> out;
        std::string error;
        const auto reply = askDaemon(daemonPath, mtd::ListSessions{}, error, 500);
        const auto *list = reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr;
        if (!list) {
            if (!quiet) {
                notify("daemon: " + (error.empty() ? std::string("unexpected answer") : error));
            }
            return out;
        }
        std::set<std::string> mine;
        workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
            if (pane.daemon && !pane.daemon->sessionId().empty()) {
                mine.insert(pane.daemon->sessionId());
            }
        });
        for (const auto &entry : list->sessions) {
            AttachableSession s;
            s.id = entry.id;
            s.attached = entry.attached;
            s.shared = entry.shared;
            s.inThisWindow = mine.count(entry.id) != 0;
            if (!s.inThisWindow) {
                std::string infoError;
                const auto info = askDaemon(daemonPath, mtd::GetSessionInfo{entry.id}, infoError, 500);
                if (const auto *got = info ? std::get_if<mtd::SessionInfo>(&*info) : nullptr) {
                    s.cwd = got->cwd;
                    s.command = got->command;
                    s.name = got->name;
                }
            }
            out.push_back(std::move(s));
        }
        return out;
    };
    // A new tab that shows an existing daemon terminal.
    const auto attachSession = [&](const std::string &id) {
        nextAttachId = id;
        const Outcome outcome = workspace.apply(Command::NewTab);
        nextAttachId.clear();
        if (outcome == Outcome::Ignored) {
            notify("no room for another tab");
            return false;
        }
        return true;
    };
    const auto openPalette = [&] {
        std::vector<CommandEntry> extra = {
            {"session.open", "Open session...", "Choose a saved session to open", std::nullopt, {}, {}},
            {"session.save", "Save session as...", "Save the tabs and panes under a name", std::nullopt, {}, {}},
            {"tab.rename", "Rename tab...", "Give the tab a name that stays", std::nullopt, {}, {}},
        };
        for (auto &entry : sessionCommandEntries()) {
            extra.push_back(std::move(entry));
        }
        for (auto &entry :
             daemonCommandEntries(daemonOptions.enabled, remoteDaemon)) {
            extra.push_back(std::move(entry));
        }
        if (daemonOptions.enabled) {
            AttachState attach;
            attach.panesInTab = activeTabPanes().size();
            const NativePane *focused = workspace.focusedPane();
            attach.focusedInDaemon = focused && focused->daemon && !focused->daemon->sessionId().empty();
            for (const auto &s : daemonSessions()) {
                if (!s.attached && !s.inThisWindow) {
                    ++attach.leftover;
                }
            }
            attach.layouts = layoutFiles.list().size();
            if (const auto *tab = workspace.activeTab()) {
                for (const PaneId paneId : tab->tree.ids()) {
                    const NativePane *p = workspace.pane(paneId);
                    attach.tabHasDaemonPane = attach.tabHasDaemonPane || (p && p->daemon);
                }
            }
            for (auto &entry : attachCommandEntries(attach)) {
                extra.push_back(std::move(entry));
            }
            ShareState state;
            state.panes = activeTabPanes().size();
            state.shared = std::size_t(wv.sharedInActiveTab());
            const NativePane *focusedPane = workspace.focusedPane();
            state.focusedShared = focusedPane && focusedPane->shared;
            for (auto &entry : shareCommandEntries(state)) {
                extra.push_back(std::move(entry));
            }
        }
        wv.pushLayer(std::make_unique<ui::CommandPalette>(buildCommandRegistry(viewConfig.prefix, extra), true));
    };
    const auto handleAction = [&](const std::string &action) {
        const auto colon = action.find(':');
        const std::string id = action.substr(0, colon);
        const std::string argument = colon == std::string::npos ? std::string() : action.substr(colon + 1);
        if (id == "window.close") {
            requestQuit(); // The same question as the window's own close button.
        }
        else if (id == "window.minimize") {
            SDL_MinimizeWindow(window.get());
        }
        else if (id == "window.maximize") {
            borderless::toggleMaximize(window.get());
        }
        else if (id == "close.confirm" || id == "close.cancel") {
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
            wv.pushLayer(std::make_unique<ui::TextPrompt>("Save session as", sessionName(), "session.save:", false));
        }
        else if (id == "session.save") {
            if (!SessionSnapshot::validName(argument)) {
                notify("session names use letters, digits, '.', '_' and '-'");
            }
            else {
                if (!autoSavedName.empty() && autoSavedName != argument) {
                    sessionFiles.remove(autoSavedName); // Its own name now: the automatic file goes.
                }
                autoSavedName.clear();
                sessionPolicy.adopt(argument);
                saveSession(argument, false);
            }
        }
        else if (id == sessionaction::remove && colon == std::string::npos) {
            std::vector<ui::PickList::Item> items;
            for (const auto &info : sessionFiles.list()) {
                items.push_back({info.name,
                                 std::to_string(info.tabs) + (info.tabs == 1 ? " tab, " : " tabs, ") +
                                     std::to_string(info.panes) + (info.panes == 1 ? " pane" : " panes"),
                                 std::string(sessionaction::remove) + ":" + info.name});
            }
            wv.pushLayer(std::make_unique<ui::PickList>("Delete session", std::move(items), "No saved sessions"));
        }
        else if (id == sessionaction::remove) {
            wv.pushLayer(deleteSessionDialog(argument, sessionPolicy.persistent() && sessionName() == argument));
        }
        else if (id == sessionaction::removeConfirm) {
            if (!SessionSnapshot::validName(argument)) {
                notify("not a session name");
            }
            else if (!sessionFiles.remove(argument)) {
                notify("session '" + argument + "' could not be deleted");
            }
            else {
                if (sessionPolicy.persistent() && sessionName() == argument) {
                    sessionPolicy.forget(); // Not written again (at quit, say) until a tab is named.
                    autoSavedName.clear();
                }
                notify("session '" + argument + "' deleted");
            }
        }
        else if (id == sessionaction::removeCancel) {
            // The dialog closed; nothing else to do.
        }
        else if (id == daemonaction::reconnect) {
            if (!reconnector) {
                notify("Reconnect is for --daemon-host");
            }
            else if (reconnector->retryNow(SDL_GetTicks64())) {
                notify("Reconnecting to " + daemonOptions.host + " ...");
            }
            else {
                notify("Already reconnecting to " + daemonOptions.host);
            }
        }
        else if (id == daemonaction::stop || id == daemonaction::status) {
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::GetStatus{}, error);
            const auto *status = reply ? std::get_if<mtd::Status>(&*reply) : nullptr;
            if (!status) {
                notify("daemon: " + (error.empty() ? std::string("unexpected answer") : error));
            }
            else if (id == daemonaction::status) {
                notify(formatDaemonStatus({true, daemonPath, status->sessions, status->attached}));
            }
            else {
                daemonStopCount = status->sessions;
                wv.pushLayer(stopDaemonDialog(daemonStopCount));
            }
        }
        else if (id == daemonaction::stopConfirm) {
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::Shutdown{daemonStopCount}, error);
            const auto *done = reply ? std::get_if<mtd::ShutdownReply>(&*reply) : nullptr;
            if (!done) {
                notify("daemon: " + (error.empty() ? std::string("unexpected answer") : error));
            }
            else if (!done->done) {
                // The number of programs changed while the question was open: ask again.
                daemonStopCount = done->sessions;
                wv.pushLayer(stopDaemonDialog(daemonStopCount));
            }
            else {
                notify("daemon stopped");
            }
        }
        else if (id == daemonaction::sharePane || id == daemonaction::sharePaneStop) {
            if (const auto focusedId = workspace.focusedPaneId(); focusedId != 0) {
                setShared({focusedId}, id == daemonaction::sharePane);
            }
        }
        else if (id == daemonaction::shareTab || id == daemonaction::shareTabStop) {
            setShared(activeTabPanes(), id == daemonaction::shareTab);
        }
        else if (id == daemonaction::renamePane && colon == std::string::npos) {
            if (const NativePane *focusedPane = workspace.focusedPane(); focusedPane && focusedPane->daemon) {
                wv.pushLayer(std::make_unique<ui::TextPrompt>("Rename terminal", focusedPane->name,
                                                              std::string(daemonaction::renamePane) + ":", true));
            }
        }
        else if (id == daemonaction::renamePane) {
            NativePane *focusedPane = workspace.focusedPane();
            if (!focusedPane || !focusedPane->daemon || focusedPane->daemon->sessionId().empty()) {
                // Nothing to name.
            }
            else if (!mtd::validTerminalName(argument)) {
                notify("names are at most 64 bytes, without control characters");
            }
            else {
                std::string error;
                const auto reply = askDaemon(daemonPath, mtd::SetName{focusedPane->daemon->sessionId(), argument}, error);
                if (const auto *list = reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr) {
                    for (const auto &entry : list->sessions) {
                        if (entry.id == focusedPane->daemon->sessionId()) {
                            focusedPane->name = entry.name;
                        }
                    }
                    notify(argument.empty() ? "terminal name removed" : "terminal named '" + argument + "'");
                }
                else {
                    const auto *refused = reply ? std::get_if<mtd::ControlError>(&*reply) : nullptr;
                    notify("daemon: " + (refused ? refused->reason : error.empty() ? std::string("could not name it (an older daemon?)") : error));
                }
            }
        }
        else if (id == daemonaction::attachHere) {
            wv.pushLayer(std::make_unique<ui::PickList>("Attach terminal here",
                                                        attachItems(daemonSessions(false), daemonaction::attachHereGo),
                                                        "No other terminals in the daemon"));
        }
        else if (id == daemonaction::attachHereGo) {
            const bool force = argument.size() > 6 && argument.compare(argument.size() - 6, 6, ":force") == 0;
            const std::string sessionId = force ? argument.substr(0, argument.size() - 6) : argument;
            const auto sessions = daemonSessions(false);
            const auto found = std::find_if(sessions.begin(), sessions.end(),
                                            [&](const AttachableSession &s) { return s.id == sessionId; });
            NativePane *here = workspace.focusedPane();
            if (found == sessions.end()) {
                notify("that terminal is gone");
            }
            else if (found->inThisWindow || !here || !here->daemon) {
                notify("this window already shows that terminal");
            }
            else if (found->attached && !force) {
                wv.pushLayer(takeOverDialog(sessionId, shortFolder(found->cwd), daemonaction::attachHereGo));
            }
            else {
                const auto state = paneState(*here);
                wv.pushLayer(replaceTerminalDialog(sessionId, state.busy ? commandLine(state.command) : std::string()));
            }
        }
        else if (id == daemonaction::attachHereDo) {
            const auto colon2 = argument.rfind(':');
            const std::string sessionId = colon2 == std::string::npos ? argument : argument.substr(0, colon2);
            const bool endOld = colon2 == std::string::npos || argument.substr(colon2 + 1) == "end";
            if (NativePane *here = workspace.focusedPane(); here && here->daemon) {
                here->swapTerminal(sessionId, endOld);
                notify(endOld ? "terminal replaced" : "terminal replaced; the old one keeps running in the daemon");
            }
        }
        else if (id == daemonaction::killPane) {
            if (NativePane *here = workspace.focusedPane(); here && here->daemon) {
                const auto state = paneState(*here);
                wv.pushLayer(killTerminalDialog(state.busy ? commandLine(state.command) : std::string()));
            }
        }
        else if (id == daemonaction::killConfirm) {
            if (const PaneId focusedId = workspace.focusedPaneId(); focusedId != 0) {
                // Closing a daemon pane ends its terminal (endInDaemon is true unless it was detached).
                if (NativePane *here = workspace.pane(focusedId)) {
                    here->endInDaemon = true;
                }
                if (workspace.closePaneAnywhere(focusedId) == Outcome::Empty) {
                    running = false;
                }
                notify("terminal ended");
            }
        }
        else if (id == daemonaction::detachTab) {
            const auto *tab = workspace.activeTab();
            auto snapshot = captureSession(
                workspace, "layout", std::int64_t(std::time(nullptr)), [&](PaneId, const NativePane &pane) {
                    SavedPane saved;
                    const auto state = paneState(const_cast<NativePane &>(pane));
                    saved.cwd = state.cwd;
                    saved.command = state.command;
                    if (pane.daemon && !pane.daemon->sessionId().empty() &&
                        LayoutSnapshot::validId(pane.daemon->sessionId())) {
                        saved.terminal = pane.daemon->sessionId();
                    }
                    return saved;
                });
            if (!tab || !snapshot || snapshot->active >= snapshot->tabs.size()) {
                notify("nothing to detach");
            }
            else {
                // Only the active tab; named after its title, a free name when it is taken.
                SavedTab only = std::move(snapshot->tabs[snapshot->active]);
                snapshot->tabs.clear();
                snapshot->tabs.push_back(std::move(only));
                snapshot->active = 0;
                const std::string title = workspace.userTitle(workspace.activeIndex());
                std::string base = SessionPolicy("layout").suggestion(title); // The tab's name as a file name.
                if (title.empty()) {
                    base = "layout";
                }
                std::string name = base;
                for (int n = 2; layoutFiles.load(name) && n < 100; ++n) {
                    name = base.substr(0, SessionSnapshot::kMaxName - 4) + "-" + std::to_string(n);
                }
                snapshot->name = name;
                std::string error;
                if (!layoutFiles.save(*snapshot, &error)) {
                    notify("layout not saved: " + error);
                }
                else {
                    // The terminals stay in the daemon; the tab goes.
                    for (const PaneId paneId : workspace.activeTab()->tree.ids()) {
                        if (NativePane *p = workspace.pane(paneId)) {
                            p->endInDaemon = false;
                        }
                    }
                    if (workspace.apply(Command::CloseTab) == Outcome::Empty) {
                        running = false;
                    }
                    notify("tab detached as layout '" + name + "' (Attach layout... opens it again)");
                }
            }
        }
        else if (id == daemonaction::attachLayout && colon == std::string::npos) {
            std::vector<ui::PickList::Item> items;
            for (const auto &info : layoutFiles.list()) {
                items.push_back({info.name,
                                 std::to_string(info.panes) + (info.panes == 1 ? " terminal" : " terminals"),
                                 std::string(daemonaction::attachLayout) + ":" + info.name});
            }
            wv.pushLayer(std::make_unique<ui::PickList>("Attach layout", std::move(items), "No detached layouts"));
        }
        else if (id == daemonaction::attachLayout || id == daemonaction::attachLayoutGo) {
            const bool force = id == daemonaction::attachLayoutGo && argument.size() > 6 &&
                               argument.compare(argument.size() - 6, 6, ":force") == 0;
            const std::string layoutName = force ? argument.substr(0, argument.size() - 6) : argument;
            const auto snapshot = SessionSnapshot::validName(layoutName) ? layoutFiles.load(layoutName) : std::nullopt;
            if (!snapshot || snapshot->tabs.empty()) {
                notify("layout '" + layoutName + "' cannot be read");
            }
            else {
                const auto plan = planLayoutAttach(snapshot->tabs[0], daemonSessions(false));
                if (plan.takeOver > 0 && !force) {
                    wv.pushLayer(takeOverLayoutDialog(layoutName, plan.takeOver));
                }
                else {
                    // Panes are made in the restore's own order, so a terminal is found by its id.
                    std::set<std::string> usable;
                    for (const auto &u : plan.use) {
                        if (!u.empty()) usable.insert(u);
                    }
                    const auto report = restoreSession(workspace, *snapshot, [&](const SavedPane &saved) {
                        nextAttachId.clear();
                        if (!saved.terminal.empty() && usable.erase(saved.terminal) != 0) {
                            nextAttachId = saved.terminal; // Once: a terminal named twice is shown once.
                        }
                        std::error_code ec;
                        nextCwd = !saved.cwd.empty() && std::filesystem::is_directory(saved.cwd, ec) ? saved.cwd : std::string();
                    });
                    nextAttachId.clear();
                    nextCwd.clear();
                    for (const auto &restored : report.panes) {
                        NativePane *p = workspace.pane(restored.id);
                        if (p && restored.saved && !restored.saved->terminal.empty() && !p->daemon) {
                            continue;
                        }
                        if (p && restored.saved && !restored.saved->terminal.empty() &&
                            p->daemon->sessionId() != restored.saved->terminal) {
                            p->v.note("its terminal is gone: a new one was started here");
                        }
                    }
                    if (report.tabs == 0) {
                        notify("no room for the layout");
                    }
                    else {
                        layoutFiles.remove(layoutName); // It is attached now, no longer detached.
                        notify("layout '" + layoutName + "' attached");
                    }
                }
            }
        }
        else if (id == daemonaction::attach) {
            wv.pushLayer(std::make_unique<ui::PickList>("Attach terminal", attachItems(daemonSessions(false)),
                                                        "No other terminals in the daemon"));
        }
        else if (id == daemonaction::attachGo) {
            const bool force = argument.size() > 6 && argument.compare(argument.size() - 6, 6, ":force") == 0;
            const std::string sessionId = force ? argument.substr(0, argument.size() - 6) : argument;
            const auto sessions = daemonSessions(false);
            const auto found = std::find_if(sessions.begin(), sessions.end(),
                                            [&](const AttachableSession &s) { return s.id == sessionId; });
            if (found == sessions.end()) {
                notify("that terminal is gone");
            }
            else if (found->inThisWindow) {
                notify("this window already shows that terminal");
            }
            else if (found->attached && !force) {
                wv.pushLayer(takeOverDialog(sessionId, shortFolder(found->cwd)));
            }
            else {
                attachSession(sessionId);
            }
        }
        else if (id == daemonaction::attachAll) {
            std::size_t count = 0;
            for (const auto &s : daemonSessions(false)) {
                count += !s.attached && !s.inThisWindow;
            }
            if (count == 0) {
                notify("no leftover terminals");
            }
            else {
                wv.pushLayer(attachAllDialog(count));
            }
        }
        else if (id == daemonaction::attachAllGo) {
            for (const auto &s : daemonSessions(false)) {
                if (!s.attached && !s.inThisWindow && !attachSession(s.id)) {
                    break;
                }
            }
        }
        else if (id == daemonaction::attachCancel) {
            // Nothing to do: the dialog closed.
        }
        else if (id == daemonaction::detachPane || id == daemonaction::movePane) {
            const PaneId focusedId = workspace.focusedPaneId();
            NativePane *focusedPane = workspace.focusedPane();
            if (focusedPane && focusedPane->daemon && !focusedPane->daemon->sessionId().empty()) {
                const std::string sessionId = focusedPane->daemon->sessionId();
                if (id == daemonaction::movePane && !attachSession(sessionId)) {
                    // No room: the pane stays where it is.
                }
                else {
                    if (NativePane *old = workspace.pane(focusedId)) {
                        old->endInDaemon = false; // Closing the pane must not end the program.
                    }
                    if (workspace.closePaneAnywhere(focusedId) == Outcome::Empty) {
                        running = false;
                    }
                    if (id == daemonaction::detachPane) {
                        notify("terminal left running in the daemon (Attach terminal... shows it again)");
                    }
                }
            }
        }
        else if (id == daemonaction::quitKeep) {
            keepTerminals = true;
            running = false;
        }
        else if (id == "tab.rename" && colon == std::string::npos) {
            wv.pushLayer(std::make_unique<ui::TextPrompt>("Rename tab", workspace.userTitle(workspace.activeIndex()), "tab.rename:", true));
        }
        else if (id == "tab.rename") {
            workspace.setUserTitle(workspace.activeIndex(), SessionSnapshot::cleanTitle(argument));
            if (sessionPolicy.shouldSave(SessionPolicy::Event::TabNamed)) {
                autoSave(false);
            }
        }
    };
    // What the layout says besides its own changes: a palette to open, an answer from an overlay,
    // a question before closing.
    const auto layoutExtras = [&](const auto &r) {
        if (r.openPalette) {
            openPalette();
        }
        if (r.copyMode) {
            enterCopyMode();
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
    const auto pushMouse = [&](Uint32 type, int viewX, int y) {
        // The script works in view pixels; events are in window pixels (the border is outside the view).
        const int x = viewX + insetNow().left;
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
            expectThat(pane && !pane->finished(),
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

    constexpr Uint64 kActiveMs = 500, kBlinkTimeoutMs = 10000;
    Uint64 lastActivity = SDL_GetTicks64();
    Uint64 activeUntil = SDL_GetTicks64() + kActiveMs;
    std::uint64_t lastSteadySignature = 0, lastDrawnSignature = 0;
    bool forceRedraw = true; // The first frame; again after anything that may have disturbed the window.
    std::string shownTitle;
    std::uint64_t framesDrawn = 0, iterations = 0;
    const Uint64 loopStarted = SDL_GetTicks64();
    while (running) {
        ++iterations;
        skipText = false;
        if (gShutdownSignal != 0) {
            keepTerminals = true; // The terminal or session went away, not the owner's wish to end them.
            break; // SIGTERM or SIGHUP: leave through the shutdown save below, no question asked.
        }
        if (wv.pendingClose() && !wv.hasLayer()) {
            wv.answerClose(CloseAnswer::Cancel); // The dialog was dismissed by a click outside.
        }
        updateWindow();
        script();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            activeUntil = SDL_GetTicks64() + kActiveMs;
            lastActivity = SDL_GetTicks64();
            if (event.type == SDL_WINDOWEVENT) {
                forceRedraw = true; // Exposed, resized, shown: the window's contents may be gone.
            }
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
                else if (auto *copying = workspace.focusedPane(); copying && copying->v.copyMode) {
                    // One chord per ASCII character; anything else is ignored.
                    for (const char c : std::string_view(event.text.text)) {
                        if (copying->v.copyMode && c >= 32 && c < 127) {
                            copyKey(*copying, {int(c), false, false, false});
                        }
                    }
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
                if (ownFrame && motion) {
                    showEdgeCursor(frontend::cursorShapeFor(hitState.classify(event.motion.x, event.motion.y)));
                }
                MouseResult r;
                guarded([&] { r = wv.mouse(in, now); });
                if (r.empty) {
                    running = false;
                }
                layoutExtras(r);
                if (r.cursor != shownCursor) {
                    shownCursor = r.cursor;
                    if (edgeShape == frontend::CursorShape::Default) {
                        SDL_SetCursor(appCursor());
                    }
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
            else if (ownFrame && event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_LEAVE) {
                showEdgeCursor(frontend::CursorShape::Default); // The pointer left the window.
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
                if (np.v.copyMode) {
                    // The program gets nothing. Printable keys arrive as text (below); named keys
                    // and Ctrl+letter are fed here.
                    if (code != 0 && (!printable || mod.control)) {
                        copyKey(np, {code, mod.control, mod.shift, mod.alt});
                    }
                    continue;
                }
                if (key == SDLK_RETURN && !mod.control && !mod.alt &&
                    !mod.shift && np.inDaemon()) {
                    using State = frontend::TunnelReconnector::State;
                    if (reconnector &&
                        reconnector->state() != State::Connected &&
                        reattachKind(np.v) != Reattach::None) {
                        // The connection itself is down: retry it now (the
                        // panes attach when it is up).
                        notify(reconnector->retryNow(SDL_GetTicks64())
                                   ? "Reconnecting to " + daemonOptions.host +
                                         " ..."
                                   : "Already reconnecting to " +
                                         daemonOptions.host);
                        continue;
                    }
                    if (np.reattach()) {
                        // Connected, yet the daemon cannot be reached: the ssh
                        // may be stuck; replace it.
                        if (reconnector &&
                            np.v.state() == TerminalView::State::Disconnected &&
                            reconnector->retryNow(SDL_GetTicks64())) {
                            notify("Reconnecting to " + daemonOptions.host +
                                   " ...");
                        }
                        continue; // Enter in a pane that lost its terminal:
                                  // attach again.
                    }
                }
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
        if (reconnector) {
            using Event = frontend::TunnelReconnector::Event;
            const auto host = daemonOptions.host;
            switch (reconnector->tick(SDL_GetTicks64())) {
            case Event::Lost:
                notify("The connection to " + host +
                       " ended; the terminals there keep running. Reconnecting "
                       "...");
                break;
            case Event::Reconnected:
                notify("Connected to " + host + " again");
                workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                    // A pane taken over by someone else stays as it is; only
                    // lost connections come back.
                    if (pane.inDaemon() &&
                        pane.v.state() == TerminalView::State::Disconnected) {
                        pane.reattach();
                    }
                });
                break;
            case Event::GaveUp:
                notify("Could not reach " + host + " (" +
                       reconnector->lastError() +
                       "). Enter in the pane or Reconnect retries.");
                workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                    if (pane.inDaemon() &&
                        pane.v.state() == TerminalView::State::Disconnected) {
                        pane.v.note("disconnected from " + host +
                                    ", press Enter to retry");
                    }
                });
                break;
            case Event::None:
                break;
            }
        }
        const bool daemonReachable =
            !reconnector || reconnector->state() ==
                                frontend::TunnelReconnector::State::Connected;
        // A slow link: ask less often (each question waits for the answer).
        if (daemonOptions.enabled && daemonReachable &&
            SDL_GetTicks64() - lastSharedPoll >=
                (remoteDaemon ? 10000u : 2000u)) {
            // The shared marks are the daemon's; another tool may have changed them.
            lastSharedPoll = SDL_GetTicks64();
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::ListSessions{}, error, 200);
            if (const auto *list = reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr) {
                workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                    if (!pane.daemon) {
                        return;
                    }
                    for (const auto &entry : list->sessions) {
                        if (entry.id == pane.daemon->sessionId()) {
                            pane.shared = entry.shared;
                            pane.name = entry.name;
                    pane.name = entry.name;
                        }
                    }
                });
            }
        }
        // Every process keeps being served, shown or not; ended ones close their pane.
        std::vector<PaneId> ended;
        workspace.forEachPane([&](PaneId id, NativePane &pane, bool) {
            pane.pump();
            if (pane.inDaemon()) {
                const bool taken = pane.v.state() == TerminalView::State::Replaced;
                if (taken && !pane.takeoverReported) {
                    notify(takenOverNotice(pane.shared, pane.name));
                }
                pane.takeoverReported = taken;
            }
            if (pane.finished()) {
                ended.push_back(id);
            }
        });
        for (const PaneId id : ended) {
            if (auto *pane = workspace.pane(id)) {
                lastExit = pane->exitStatus();
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

        // Everything that is drawn is collected first, so that a window where nothing changed is not
        // drawn at all (an idle terminal costs no CPU; see docs/CPU.md).
        // Blinking stops (visible) after ten seconds without input or change, like GNOME's
        // gtk-cursor-blink-timeout: a resting window then draws nothing at all.
        const bool blinkOn = now >= lastActivity + kBlinkTimeoutMs || (now / 500) % 2 == 0;
        if (ownFrame) {
            wv.setWindowMaximized((SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) != 0);
        }
        const FramePlan plan = wv.plan();
        if (ownFrame) { // What SDL's hit test reads: the handle is the strip's gaps.
            hitState.set(borderless::geometryOf(
                window.get(), plan.strip, plan.stripRect.h, backend.cellWidth(), drawableSize().first, insetNow().left));
        }
        const Palette stripPalette;
        std::optional<CellBuffer> stripBuffer;
        std::optional<RenderFrame> stripFrame;
        if (plan.stripRect.h > 0) {
            stripBuffer.emplace(plan.strip.cells.size(), 1);
            for (std::size_t i = 0; i < plan.strip.cells.size(); ++i) {
                stripBuffer->at(i, 0) = plan.strip.cells[i];
            }
            stripFrame.emplace();
            stripFrame->cells = stripBuffer->view();
            stripFrame->cursor.visible = false;
            stripFrame->palette = &stripPalette;
        }
        int pointerX = 0, pointerY = 0;
        {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            std::tie(pointerX, pointerY) = toDrawable(mx, my);
        }
        const auto mouseMod = modifiers(SDL_GetModState());
        std::optional<UrlMatch> shownLink;
        struct Drawn {
            const PanePlan *plan = nullptr;
            RenderFrame frame;
            const Palette *palette = nullptr;
            bool steadyCursorVisible = true;
            std::unique_ptr<CellBuffer> badge; // Copy mode: COPY / SELECT and where the cursor is.
            RenderFrame badgeFrame;
            PixelRect badgeRect;
            Palette badgePalette;
        };
        std::deque<Drawn> drawn; // Stable addresses: the frames point into their buffers.
        for (const auto &pp : plan.panes) {
            NativePane *pane = workspace.pane(pp.id);
            if (!pane) {
                continue;
            }
            auto &model = pane->v.model();
            model.advanceViewport(seconds);
            auto frame = model.frame(backend.cellHeight());
            frame.palette = &pane->v.palette();
            frame.wrapMarks =
                model.viewWrapMarks(); // The wrapped-line marker
                                       // (docs/tasks/wrap-marker.md).
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
            frame.textBlinkVisible = blinkOn;
            auto &glide = pane->v.glide();
            glide.update(frame.cursor.column,
                         frame.cursor.row,
                         frame.cursor.visible,
                         double(now) / 1000);
            glide.apply(frame.cursor);
            frame.cursor.focused = windowFocused && pp.focused;
            const bool steadyCursorVisible = frame.cursor.visible;
            frame.cursor.visible = frame.cursor.visible &&
                                   (!frame.cursor.focused ||
                                    !frame.cursor.blink ||
                                    blinkOn);
            if (pane->v.copyMode) {
                pane->v.copyMode->apply(model, frame); // The virtual cursor replaces the real one.
            }
            Drawn &d = drawn.emplace_back();
            d.plan = &pp;
            d.frame = std::move(frame);
            d.palette = &pane->v.palette();
            d.steadyCursorVisible = steadyCursorVisible;
            if (pane->v.copyMode) {
                const std::string text = pane->v.copyMode->badge(model);
                d.badge = std::make_unique<CellBuffer>(text.size(), 1);
                for (std::size_t i = 0; i < text.size(); ++i) {
                    d.badge->at(i, 0) = FChar{char32_t(text[i]), Rgb8{20, 20, 20}, Rgb8{255, 190, 60}};
                }
                d.badgeFrame.cells = d.badge->view();
                d.badgeFrame.cursor.visible = false;
                d.badgeFrame.palette = &d.badgePalette;
                const int width = int(text.size()) * backend.cellWidth();
                const int x = pp.rect.x + pp.rect.w - width - SdlBackend::scrollbarStrip();
                d.badgeRect = x >= pp.rect.x ? PixelRect{x, pp.rect.y, width, backend.cellHeight()} : PixelRect{};
            }
        }
        std::string title = "matterm";
        if (const auto *focused = workspace.focusedPane();
            focused && !focused->v.title().empty()) {
            title += " | " + focused->v.title();
        }
        if (sessionPolicy.persistent()) {
            title += " | session " + sessionName();
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
        // Did anything change? `full` is every pixel (the blink phase too); `steady` leaves the blink out,
        // so a blinking cursor alone lets the loop rest between its two redraws a second.
        std::vector<frontend::PaneSignatureInputEx> inputs;
        const auto addInput = [&](const RenderFrame &frame, const Palette *palette, const PixelRect &rect,
                                  const PixelRect &scrollbar, bool steadyCursor) {
            frontend::PaneSignatureInputEx in;
            in.frame = &frame;
            in.palette = palette;
            in.rect = rect;
            in.scrollbar = scrollbar;
            in.steadyCursorVisible = steadyCursor;
            inputs.push_back(in);
        };
        if (stripFrame) {
            addInput(*stripFrame, &stripPalette, plan.stripRect, PixelRect{}, false);
        }
        std::vector<RenderFrame> layerFrames;
        layerFrames.reserve(plan.layers.size());
        for (const auto &layer : plan.layers) {
            RenderFrame frame;
            frame.cells = layer.cells->view();
            frame.cursor = layer.cursor;
            frame.cursor.focused = true;
            frame.palette = &stripPalette;
            layerFrames.push_back(std::move(frame));
        }
        for (const auto &d : drawn) {
            addInput(d.frame, d.palette, d.plan->rect, d.plan->scrollbar, d.steadyCursorVisible);
            if (d.badge) {
                addInput(d.badgeFrame, &d.badgePalette, d.badgeRect, PixelRect{}, false);
            }
        }
        for (std::size_t i = 0; i < layerFrames.size(); ++i) {
            addInput(layerFrames[i], &stripPalette, plan.layers[i].rect, PixelRect{}, layerFrames[i].cursor.visible);
        }
        std::uint64_t extra = std::hash<std::string>{}(title);
        for (const auto &d : drawn) {
            extra = extra * 31 + (d.plan->shared ? 1u : 0u);
        }
        {
            int w = 0, h = 0;
            SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()), &w, &h);
            extra = extra * 31 + std::uint64_t(w) * 65537 + std::uint64_t(h);
            extra = extra * 31 + (ownFrame && (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) ? 1u : 0u);
        }
        const auto signatures = frontend::windowSignatures(inputs, plan.dividers, extra);
        if (signatures.steady != lastSteadySignature) {
            lastSteadySignature = signatures.steady;
            activeUntil = now + kActiveMs;
            lastActivity = now;
        }
        if (forceRedraw || signatures.full != lastDrawnSignature) {
            forceRedraw = false;
            lastDrawnSignature = signatures.full;
            if (title != shownTitle) {
                shownTitle = title;
                SDL_SetWindowTitle(window.get(), title.c_str());
            }
            const int ox = insetNow().left; // Where the view starts: after the border.
            backend.beginFrame(stripPalette.background);
            if (stripFrame) {
                backend.render(*stripFrame,
                               PaneArea{{plan.stripRect.x + ox, plan.stripRect.y, plan.stripRect.w, plan.stripRect.h}, {}});
            }
            for (const auto &d : drawn) {
                const auto &pp = *d.plan;
                backend.render(d.frame,
                               PaneArea{{pp.rect.x + ox, pp.rect.y, pp.rect.w, pp.rect.h},
                                        {pp.scrollbar.x + ox, pp.scrollbar.y, pp.scrollbar.w, pp.scrollbar.h}});
                if (d.badge && d.badgeRect.w > 0) {
                    backend.render(d.badgeFrame,
                                   PaneArea{{d.badgeRect.x + ox, d.badgeRect.y, d.badgeRect.w, d.badgeRect.h}, {}});
                }
                if (pp.shared) {
                    // Shared with browsers: a small red square in the pane's corner.
                    const int side = std::max(4, backend.cellHeight() / 3);
                    backend.drawRect({pp.rect.x + ox + pp.rect.w - side - 2, pp.rect.y + 2, side, side}, Rgb8{210, 40, 40});
                }
            }
            for (const auto &d : plan.dividers) {
                backend.drawRect({d.rect.x + ox, d.rect.y, d.rect.w, d.rect.h}, Rgb8{46, 51, 60});
            }
            for (std::size_t i = 0; i < layerFrames.size(); ++i) {
                const auto &layer = plan.layers[i];
                backend.render(layerFrames[i], PaneArea{{layer.rect.x + ox, layer.rect.y, layer.rect.w, layer.rect.h}, {}});
            }
            if (ownFrame) { // The window has no frame: the border (outside the view) shows where its edge is.
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()), &w, &h);
                const bool maximized = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
                for (const auto &edge : frontend::borderRects(w, h, maximized)) {
                    backend.drawRect({edge.x, edge.y, edge.w, edge.h}, frontend::borderColor(stripPalette));
                }
            }
            backend.present();
            ++framesDrawn;
            // At most about 120 frames a second even where presenting does not wait for the display.
            if (const auto spent = SDL_GetTicks64() - now; spent < 8) {
                SDL_WaitEventTimeout(nullptr, int(8 - spent));
            }
        }
        else {
            // Nothing to draw: rest until something can happen. Input wakes us at once; output of the
            // programs is picked up by the next round (4 ms while things are happening, 100 ms when it
            // has been quiet for half a second).
            SDL_WaitEventTimeout(nullptr, now < activeUntil ? 4 : 100);
        }
    }
    SDL_StopTextInput();
    if (std::getenv("MATTERM_LOOP_STATS")) {
        // For finding out why a window uses CPU: rounds of the loop and frames actually drawn.
        const double seconds = double(SDL_GetTicks64() - loopStarted) / 1000;
        std::cerr << "matterm: " << iterations << " loop rounds, " << framesDrawn << " frames drawn in " << seconds
                  << " s\n";
    }
    if (keepTerminals) {
        workspace.forEachPane([](PaneId, NativePane &pane, bool) { pane.endInDaemon = false; });
    }
    // A controlled shutdown (quit, the window's close button, SIGTERM, SIGHUP) writes the session
    // of a window the owner made persistent, with a checkpoint of what is on the screens. Nothing
    // is written for a window whose session was never saved, and nothing is ever restored on start.
    if (sessionPolicy.shouldSave(SessionPolicy::Event::Shutdown)) {
        autoSave(sessionPolicy.withScreenText(SessionPolicy::Event::Shutdown));
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
