#include "ptyfrontend.h"
#include "attachreplacement.h"
#include "daemonconnect.h"
#include "emptyshell.h"
#include "restorecwd.h"
#include "restoreprograms.h"
#include "ui/programlist.h"

#include "borderless.h"

#include "cellbuffer.h"
#include "cursorglide.h"
#include "daemonclient.h"
#include "daemoncommands.h"
#include "daemonlink.h"
#include "drawbatching.h"
#include "framesignature.h"
#include "inputencoder.h"
#include "perfclient.h"
#include "procinfo.h"
#include "ptylink.h"
#include "ptysession.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "sessioncommands.h"
#include "sessionfiles.h"
#include "sessionrestore.h"
#include "recoverylayouts.h"
#include "serverwindow.h"
#include "settingswatch.h"
#include "splitdecoration.h"
#include "sshtunnel.h"
#include "tty/utf8.h"
#include "terminalmodel.h"
#include "terminalview.h"
#include "themes.h"
#include "ui/commandpalette.h"
#include "ui/dialogs.h"
#include "workspaceview.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
#include <deque>
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
#include <sys/wait.h>
#include <utility>

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
std::size_t gScrollbackRows =
    1000; // settings.json "scrollbackRows" (for panes made from now on)

TerminalViewConfig paneViewConfig(int columns, int rows) {
    TerminalViewConfig config{
        std::size_t(columns), std::size_t(rows), gScrollbackRows, {}};
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
    double settingsStartedAt =
        0; // Opt-in diagnostics only, no callbacks retaining this pane.
    bool settingsParsed = false, settingsPresented = false;
    bool startupStatusPane = false;
    std::unique_ptr<perf::EchoMeter> perfEcho;
    perf::Second *perfSecond = nullptr;
    double perfProbeAt = 0;

    void measure(perf::Second &second) {
        perfSecond = &second;
        if (!perfEcho) {
            perfEcho = std::make_unique<perf::EchoMeter>();
            v.setParseTimer(
                [this](double ms) { perfSecond->counters.parseMs += ms; });
            v.setInputTap([this] { perfEcho->keySent(perfClockMs()); });
            v.setOutputTap([this](std::string_view) {
                perfEcho->outputArrived(perfClockMs());
                perfSecond->echo.merge(perfEcho->samples());
                perfEcho->samples().clear();
            });
        }
        if (daemon) {
            daemon->session().setTrafficObserver(
                [this](bool received, std::size_t size) {
                    // UnixSocketChannel prefixes each encoded protocol message
                    // with four bytes.
                    if (received) {
                        perfSecond->counters.in(size + 4);
                    }
                    else {
                        perfSecond->counters.out(size + 4);
                    }
                });
            if (const auto rtt = daemon->session().takeRoundTripMs()) {
                second.rtt.add(*rtt);
            }
            const double now = perfClockMs();
            if (now - perfProbeAt >= 1000) {
                daemon->session().probeRoundTrip();
                perfProbeAt = now;
            }
            ++second.clients;
        }
    }
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
    ClientPerf perfLog(daemonOptions.perfLog, "matterm-native");
    if (!perfLog.error().empty()) {
        std::cerr << "matterm: performance log: " << perfLog.error() << '\n';
    }
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
    std::optional<std::string> daemonFallbackReason; // set: the daemon was wanted and is not there
    if (daemonOptions.enabled) {
        // Finds or starts the daemon, checks its version and may switch to a
        // new daemon next to an incompatible one (daemonOptions.socketPath is
        // then the new socket).
        const auto error =
            selectDaemon(daemonOptions, remoteDaemon, !remoteDaemon);
        if (!error.empty()) {
            if (daemonOptions.required) {
                throw std::runtime_error("--daemon: " + error);
            }
            std::cerr << "matterm: no daemon (" << error << "); running the programs here\n";
            daemonOptions.enabled = false;
            daemonFallbackReason = error;
        }
    }
    if (remoteDaemon)
        frontend::sendServerStatus(daemonOptions.serverReadyFd, 'P', "Remote daemon ready; creating window...");
    const std::string daemonPath = daemonSocket(daemonOptions.socketPath);
    Uint64 lastSharedPoll = 0;
    bool keepTerminals = false;           // Leave the programs running in the daemon when we go.
    std::uint32_t daemonStopCount = 0;    // What the "Stop daemon" question named.
    OutputRecording recording(recordPath);
    bool recordFirst = recordPath != nullptr;
    int hintColumns = 80, hintRows = 24; // size of a new pane until the layout says
    // Folder the next pane's shell should start in (a restored session), else empty.
    std::string nextCwd;
    bool failEmptyProbeForTest =
        false; // Layout smoke only: metadata unavailable.
    double nextSettingsStartedAt = 0;
    bool nextSettings = false; // Settings always runs on this client, never in
                               // its remote daemon.
    bool nextServerStatus = false; // A local, disposable startup-log tab.
    bool recoveryAttachOnly = false;
    bool failNextSettingsForTest =
        false; // Layout smoke: inject a factory failure.
    // A daemon terminal the next pane should show instead of making a new one
    // (Attach terminal). For measuring a reattach (bench/replay-measure.py,
    // docs/CPU.md): the first pane shows this terminal.
    std::string nextAttachId = std::getenv("MATTERM_ATTACH_TERMINAL")
                                   ? std::getenv("MATTERM_ATTACH_TERMINAL")
                                   : "";
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
        // Consume before anything can throw: a failed Settings launch must
        // never redirect the next ordinary tab or split to local Settings.
        const bool settingsPane = std::exchange(nextSettings, false);
        const bool statusPane = std::exchange(nextServerStatus, false);
        const double settingsStartedAt =
            std::exchange(nextSettingsStartedAt, 0.0);
        if (settingsPane && std::exchange(failNextSettingsForTest, false))
            throw std::runtime_error("injected Settings factory failure");
        std::function<void(std::string_view)> record;
        if (recordFirst) {
            recordFirst = false;
            record = [&recording](std::string_view bytes) {
                recording.append(bytes);
            };
        }
        std::vector<std::string> command =
            settingsPane ? std::vector<std::string>{"/proc/self/exe", "--settings"}
            : statusPane ? std::vector<std::string>{"/bin/sleep", "86400"}
                         : arguments;
        if (!settingsPane && !statusPane && !nextCwd.empty()) {
            // cd, then become the usual command (the pane's process stays the shell, so /proc
            // and the close question see the shell). A folder that vanished was checked before.
            command = {"/bin/sh", "-c", "cd -- \"$1\" 2>/dev/null; shift; exec \"$@\"", "sh", nextCwd};
            command.insert(command.end(), arguments.begin(), arguments.end());
        }
        if (recoveryAttachOnly && nextAttachId.empty())
            throw std::runtime_error("Recovery terminal is unavailable; no shell started");
        auto pane =
            daemonOptions.enabled && !settingsPane && !statusPane
                ? std::make_unique<NativePane>(daemonPath,
                                               nextAttachId,
                                               daemonCwd(),
                                               hintColumns,
                                               hintRows)
                : std::make_unique<NativePane>(
                      command, term, hintColumns, hintRows, std::move(record));
        if (settingsPane)
            pane->settingsStartedAt = settingsStartedAt;
        pane->startupStatusPane = statusPane;
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
        const auto flags = SDL_GetWindowFlags(window.get());
        return frontend::windowInset(ownFrame,
                                     (flags & SDL_WINDOW_MAXIMIZED) != 0,
                                     (flags & SDL_WINDOW_FULLSCREEN) != 0);
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
    const char *mouseDebugEnv = std::getenv("MATTERM_DEBUG_WINDOW_MOUSE");
    const bool mouseDebug = mouseDebugEnv && *mouseDebugEnv && std::string_view(mouseDebugEnv) != "0";

    SDL_StartTextInput();
    bool running = true;
    bool rejectedInput = false;
    std::string clipboardError;
    bool copyJoinFullTuiRows = true; // settings.json "copyJoinFullTuiRows"
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
            const auto text = pane.v.selection->text(model, Selection::copyLimit, copyJoinFullTuiRows);
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
        const auto r = pane.v.copyMode->key(chord, pane.v.model(), pane.v.selection, copyJoinFullTuiRows);
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
    frontend::RecoveryLayouts recovery(layoutFiles.directory(),
        remoteDaemon ? "ssh:" + daemonOptions.host : "local:" + daemonPath);
    Uint64 recoveryPollAt = 0;
    bool recoveryWriteFailed = false;
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
        // A saved folder of a remote daemon goes to it as it is: it looks on its own computer.
        if (!nextCwd.empty()) {
            return nextCwd;
        }
        if (const NativePane *focused = workspace.focusedPane(); focused && focused->daemon) {
            const auto cwd = paneState(const_cast<NativePane &>(*focused)).cwd; // As the daemon sees it.
            if (!cwd.empty()) {
                return cwd;
            }
        }
        if (remoteDaemon) {
            return {}; // This computer's folder means nothing there: its shell starts in its own home.
        }
        std::error_code ec;
        const auto here = std::filesystem::current_path(ec);
        return ec ? std::string() : here.string();
    };
    bool confirmCloseBusy = true; // settings.json "confirmClose"
    bool alignCellsBottom = false; // settings.json "cellVerticalAlignment"
    {
        // Closing a pane, tab or the window asks when a program other than the shell runs there.
        wv.setBusyProbe([&](PaneId id) -> std::optional<std::string> {
            NativePane *pane = workspace.pane(id);
            if (!pane || pane->finished() || pane->startupStatusPane || !confirmCloseBusy) {
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
    // The settings file (docs/tasks/settings.md): read now, looked at about
    // once a second, reloaded when it changed. It belongs to this computer,
    // also with --daemon-host. A theme colours the frame and tints the
    // terminals' background; a window connected to another computer names it in
    // every tab.
    settings::RealFileSystem settingsFs;
    settings::SettingsWatcher settingsWatch(
        settingsFs,
        settings::settingsPath(std::getenv("XDG_CONFIG_HOME"),
                               std::getenv("HOME")),
        [] {
            return std::int64_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        });
    const std::string computerName =
        [&] { // "user@host" is "host" to the settings and in the tabs.
            const auto at = daemonOptions.host.rfind('@');
            return at == std::string::npos ? daemonOptions.host
                                           : daemonOptions.host.substr(at + 1);
        }();
    Rgb8 themeBackground = Palette().background;
    const auto applySettings = [&] {
        const auto &current = settingsWatch.settings();
        const auto &theme =
            themes::theme(themes::themeIdFor(current, computerName));
        wv.setStripPalette(themes::stripPalette(theme));
        wv.setHostLabel(computerName);
        wv.setNamePlacement(
            parseNamePlacement(current.choice("computerNamePlacement")));
        themeBackground = themes::terminalBackground(theme);
        // The command line wins over the file where it was given (the default
        // is "icon").
        TabActivity mode = TabActivity::Icon;
        parseTabActivity(daemonOptions.tabActivity != "icon"
                             ? daemonOptions.tabActivity
                             : current.choice("tabActivity"),
                         mode);
        wv.setTabActivity(mode);
        gScrollbackRows = std::size_t(current.integer("scrollbackRows"));
        confirmCloseBusy = current.flag("confirmClose");
        alignCellsBottom = current.choice("cellVerticalAlignment") == "bottom";
        wv.setAlignBottom(alignCellsBottom);
        copyJoinFullTuiRows = current.flag("copyJoinFullTuiRows");
    };
    const auto reportSettings = [&](settings::SettingsWatcher::Event event,
                                    bool explicitReload) {
        if (event == settings::SettingsWatcher::Event::Failed) {
            notify("settings.json: " + settingsWatch.error() +
                   " (the old values stay)");
        }
        else if (event == settings::SettingsWatcher::Event::Changed ||
                 explicitReload) {
            applySettings();
            notify(settingsWatch.warnings().empty()
                       ? std::string()
                       : "settings.json: " + settingsWatch.warnings().front());
        }
    };
    if (!settingsWatch.error().empty()) {
        notify("settings.json: " + settingsWatch.error() +
               " (using the defaults)");
    }
    applySettings();
    if (!settingsWatch.warnings().empty()) {
        notify("settings.json: " + settingsWatch.warnings().front());
    }
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
    const auto canDetachClose = [&](const CloseRequest &request) {
        std::vector<PaneId> ids;
        if (request.scope == CloseScope::Tab) {
            const int index = workspace.indexOfTab(request.tab);
            if (index < 0)
                return false;
            ids = workspace.tab(std::size_t(index))->tree.ids();
        }
        else if (request.scope == CloseScope::Pane) {
            if (!workspace.pane(request.pane))
                return false;
            ids.push_back(request.pane);
        }
        else {
            workspace.forEachPane(
                [&](PaneId id, NativePane &, bool) { ids.push_back(id); });
        }
        std::vector<ClosePaneState> panes;
        for (const auto id : ids) {
            const auto *pane = workspace.pane(id);
            if (!pane)
                return false;
            panes.push_back({pane->v.ended(),
                             pane->daemon && !pane->link &&
                                 !pane->daemon->sessionId().empty()});
        }
        return windowDetachAvailable(panes);
    };
    const auto canDetachWindow = [&] {
        CloseRequest request;
        request.scope = CloseScope::Window;
        return canDetachClose(request);
    };
    const auto showCloseDialog = [&](CloseRequest request) {
        request.backgroundAvailable = canDetachClose(request);
        std::vector<ui::ConfirmDialog::Button> buttons;
        for (const auto &button : request.buttons())
            buttons.push_back({button.label, button.action});
        wv.pushLayer(
            std::make_unique<ui::ConfirmDialog>(request.question(),
                                                std::move(buttons),
                                                request.cancelIndex(),
                                                request.cancelIndex()));
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
    // Notes and typed text for a restored pane wait until its connection runs: a daemon pane that is still
    // connecting would refuse the input and lose the notes to the replay.
    struct Delivery {
        PaneId pane = 0;
        std::string note;
        std::string input; // Typed into the shell after the note; may be empty.
        Uint64 until = 0;  // Dropped after this (the pane never came up).
    };
    std::vector<Delivery> deliveries;
    const auto deliver = [&](PaneId pane, std::string note, std::string input) {
        deliveries.push_back({pane, std::move(note), std::move(input), SDL_GetTicks64() + 15000});
    };
    // The programs of the session just opened, waiting for the answer to "Start N programs again?".
    std::vector<OfferedProgram> pendingPrograms;
    const auto restoreByName = [&](const std::string &name) {
        const auto snapshot = sessionFiles.load(name);
        if (!snapshot) {
            notify("session '" + name + "' cannot be read");
            return;
        }
        std::vector<std::string> gone; // Folders that no longer exist, by creation order.
        const auto report = restoreSession(workspace, *snapshot, [&](const SavedPane &saved) {
            std::error_code ec;
            nextCwd = cwdToRequest(saved.cwd, remoteDaemon, !saved.cwd.empty() && std::filesystem::is_directory(saved.cwd, ec));
        });
        nextCwd.clear();
        std::vector<ProgramCandidate> candidates;
        pendingPrograms.clear();
        for (const auto &restored : report.panes) {
            NativePane *pane = workspace.pane(restored.id);
            if (!pane || !restored.saved) {
                continue;
            }
            const SavedPane &saved = *restored.saved;
            std::error_code ec;
            // A local pane: the folder is checked here. A pane of a remote daemon: ask where it really runs.
            const bool folderGone = remoteDaemon ? restoredElsewhere(saved.cwd, pane->daemon ? paneState(*pane).cwd : std::string())
                                                 : !saved.cwd.empty() && !std::filesystem::is_directory(saved.cwd, ec);
            if (folderGone) {
                deliver(restored.id, "restored without its folder (gone): " + CloseRequest::shown(saved.cwd), {});
            }
            if (!saved.text.empty()) {
                std::string crlf;
                for (const char c : saved.text) {
                    crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
                }
                deliver(restored.id, "previous screen:\r\n" + crlf, {});
            }
            // A program that can be started again is asked about once, below; the others (folder gone, no
            // terminal) only get the offer on the prompt line.
            ProgramCandidate candidate;
            candidate.pane = restored.id;
            candidate.saved = saved;
            candidate.folderGone = folderGone;
            candidate.noTerminal = pane->v.ended() || (daemonOptions.enabled && !pane->link && !pane->daemon);
            if (!offeredPrograms({candidate}).empty()) {
                candidates.push_back(std::move(candidate));
            }
            else if (const auto offer = offerText(saved); !offer.empty()) {
                // On the prompt line, without a newline: the owner presses Enter, edits or ignores it.
                deliver(restored.id, offer, rerunInput(saved));
            }
        }
        pendingPrograms = offeredPrograms(candidates);
        if (!pendingPrograms.empty()) {
            std::vector<std::string> lines;
            for (const auto &program : pendingPrograms) {
                lines.push_back(program.line);
            }
            wv.pushLayer(std::make_unique<ui::ProgramListDialog>(startQuestion(pendingPrograms.size()), std::move(lines),
                                                                 restoreaction::start, restoreaction::skip));
        }
        if (report.refused > 0) {
            notify("session partly restored: no room for everything");
        }
        sessionPolicy.adopt(name);
    };
    // The terminals the daemon holds, with what the list needs (a short answer or none: the
    // daemon is local). `quiet`: no notice when it does not answer.
    // The terminal this window itself runs in, when it was started inside one of the daemon's (set by the daemon).
    const char *ownSessionText = std::getenv("MATTERM_SESSION");
    const std::string ownSession = ownSessionText ? ownSessionText : "";
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
            s.thisTerminal = !ownSession.empty() && entry.id == ownSession;
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
    // The tab in front, when it is an empty shell (emptyshell.h): its pane, else 0. Asked before an attach
    // command runs; the tab is closed afterwards by closeEmptyShell().
    struct EmptyShell {
        PaneId pane = 0;
        std::size_t tabs = 0; // The tabs of the window before.
    };
    const auto emptyShellNow = [&](bool paneScope = false) -> EmptyShell {
        NativePane *pane = workspace.focusedPane();
        if (!pane || pane->v.ended()) {
            return {};
        }
        ShellFacts facts;
        facts.panesInTab = activeTabPanes().size();
        facts.inputBytes = pane->v.inputBytes();
        facts.scrollbackRows = pane->v.model().historySize();
        facts.selecting = pane->v.copyMode.has_value() || pane->v.selection.has_value();
        if (pane->daemon) {
            std::string error;
            const auto reply =
                (failEmptyProbeForTest || pane->daemon->sessionId().empty())
                    ? std::nullopt
                    : askDaemon(daemonPath,
                                mtd::GetSessionInfo{pane->daemon->sessionId()},
                                error,
                                500);
            if (const auto *info = reply ? std::get_if<mtd::SessionInfo>(&*reply) : nullptr) {
                facts.probeKnown = true;
                facts.busy = info->busy;
            }
        }
        else {
            facts.probeKnown = true;
            facts.busy = paneState(*pane).busy;
        }
        return isEmptyShell(facts, !paneScope)
                   ? EmptyShell{workspace.focusedPaneId(), workspace.tabCount()}
                   : EmptyShell{};
    };
    // After the attach worked (a new tab exists): the empty shell goes, and its fresh terminal ends in the
    // daemon (closing a pane ends its terminal). True when it was closed.
    const auto closeEmptyShell = [&](const EmptyShell &empty) {
        if (empty.pane == 0 || workspace.tabCount() <= empty.tabs) {
            return false; // Nothing to close, or the attach made no tab: never the last one.
        }
        NativePane *pane = workspace.pane(empty.pane);
        if (!pane || pane->v.inputBytes() != 0) {
            return false;
        }
        pane->endInDaemon = true;
        if (workspace.closePaneAnywhere(empty.pane) == Outcome::Empty) {
            running = false;
        }
        return true;
    };
    // What the palette offers besides the built-in commands, from the window's state right now.
    const auto paletteExtra = [&] {
        std::vector<CommandEntry> extra = {
            {"session.open",
             "Open session...",
             "Choose a saved session to open",
             std::nullopt,
             {},
             {}},
            {"session.save",
             "Save session as...",
             "Save the tabs and panes under a name",
             std::nullopt,
             {},
             {}},
            {"tab.rename",
             "Rename tab...",
             "Give the tab a name that stays",
             std::nullopt,
             {},
             {}},
            {"server.connect",
             "Connect to server...",
             "Open a separate matterm window on a saved SSH server",
             std::nullopt,
             {},
             {}},
            {"settings.open",
             "Settings",
             "Edit this computer's settings in a new local tab",
             std::nullopt,
             {},
             {}},
            {"settings.reload",
             "Reload settings",
             "Read settings.json again now",
             std::nullopt,
             {},
             {}},
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
            for (const auto &info : recovery.files().list())
                if (recovery.available(info.name)) ++attach.layouts;
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
        return extra;
    };
    struct StartingServer {
        frontend::ServerWindow process;
        PaneId pane = 0;
        std::string incoming;
        std::string installQuestion;
        Uint64 readyAt = 0;
        bool opened = false;
        bool failed = false;
        bool exited = false;
    };
    std::vector<StartingServer> serverWindows;
    std::optional<pid_t> installDialogPid;
    const auto serverStatus = [&](PaneId id, const std::string &text) {
        if (NativePane *pane = workspace.pane(id)) {
            std::string cleaned;
            for (unsigned char c : text) {
                if (c == '\n')
                    cleaned += "\r\n";
                else if (c >= 32 && c < 127)
                    cleaned += char(c);
                else if (c == '\t')
                    cleaned += ' ';
            }
            pane->v.model().write(tty::decodeUtf8(cleaned));
        }
    };
    const auto startServerWindow = [&](const std::string &host) {
        nextServerStatus = true;
        Outcome created;
        try {
            created = workspace.apply(Command::NewTab);
        }
        catch (...) {
            nextServerStatus = false;
            throw;
        }
        nextServerStatus = false;
        if (created == Outcome::Ignored) {
            notify("No room for a connection status tab");
            return;
        }
        const PaneId pane = workspace.focusedPaneId();
        workspace.setUserTitle(workspace.activeIndex(), "Connecting: " + host);
        serverStatus(pane, "Opening a separate window for " + host + "...\n");
        frontend::ServerWindow process;
        std::string error;
        if (!spawnServerWindow("/proc/self/exe", host, process, error)) {
            serverStatus(pane, "Could not start: " + error + "\n");
            notify(error);
            return;
        }
        serverWindows.push_back({process, pane});
    };
    const auto openPalette = [&] {
        wv.pushLayer(
            std::make_unique<ui::CommandPalette>(buildCommandRegistry(viewConfig.prefix, paletteExtra()), true));
    };
    struct PendingReplacement {
        PaneId pane = 0;
        NativePane *original = nullptr; // Compared for identity only; never
                                        // dereferenced after removal.
        std::string originalSession, target;
        std::unique_ptr<NativePane> staged;
        std::int64_t deadline = 0;
        bool endOld = false;
    };
    std::vector<PendingReplacement> pendingReplacements;
    const auto cancelReplacement = [&](PaneId id) {
        const auto before = pendingReplacements.size();
        pendingReplacements.erase(std::remove_if(pendingReplacements.begin(),
                                                 pendingReplacements.end(),
                                                 [&](const auto &pending) {
                                                     return pending.pane == id;
                                                 }),
                                  pendingReplacements.end());
        return before != pendingReplacements.size();
    };
    const auto stageReplacement = [&](const std::string &target, bool endOld) {
        auto *here = workspace.focusedPane();
        if (!here || !here->daemon || here->v.ended() || target.empty())
            return false;
        cancelReplacement(workspace.focusedPaneId());
        PendingReplacement pending;
        pending.pane = workspace.focusedPaneId();
        pending.original = here;
        pending.originalSession = here->daemon->sessionId();
        pending.target = target;
        pending.endOld = endOld;
        pending.deadline = std::int64_t(SDL_GetTicks64()) + 5000;
        pending.staged =
            std::make_unique<NativePane>(here->socket,
                                         target,
                                         "",
                                         int(here->v.model().columns()),
                                         int(here->v.model().rows()));
        // A cancelled/failed staging connection must NEVER end the target.
        pending.staged->endInDaemon = false;
        pendingReplacements.push_back(std::move(pending));
        notify("attaching terminal... Esc cancels; the current terminal stays "
               "until attached");
        return true;
    };
    const auto handleAction = [&](const std::string &action) {
        const auto colon = action.find(':');
        std::string id = action.substr(0, colon);
        const std::string argument = colon == std::string::npos ? std::string() : action.substr(colon + 1);
        if (id == "close.detach") {
            const auto pending = wv.pendingClose();
            wv.answerClose(CloseAnswer::Cancel);
            // Recheck the exact dialog target, not whatever was focused later.
            if (!pending || !canDetachClose(*pending))
                return;
            if (pending->scope == CloseScope::Window) {
                id = daemonaction::quitKeep;
            }
            else if (pending->scope == CloseScope::Tab) {
                workspace.apply(Command::SelectTab,
                                workspace.indexOfTab(pending->tab));
                id = daemonaction::detachTab;
            }
            else {
                workspace.focusPane(pending->pane);
                id = daemonaction::detachPane;
            }
        }
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
        else if (id == restoreaction::start || id == restoreaction::skip) {
            // The answer to "Start N programs again?": type the commands and Enter, or leave them on the
            // prompt lines as before. Panes that were closed meanwhile are skipped.
            const auto programs = std::move(pendingPrograms);
            pendingPrograms.clear();
            for (const auto &program : programs) {
                NativePane *pane = workspace.pane(program.pane);
                if (!pane) {
                    continue;
                }
                if (id == restoreaction::start) {
                    deliver(program.pane, "starting again: " + CloseRequest::shown(program.command), startInput(program));
                }
                else if (const auto offer = offerText(program.saved); !offer.empty()) {
                    deliver(program.pane, offer, rerunInput(program.saved));
                }
            }
        }
        else if (id == "settings.open") {
            nextSettings = true;
            const double startedAt = perfLog.enabled() ? perfClockMs() : 0;
            nextSettingsStartedAt = startedAt;
            perfLog.phase("settings_factory", 0, true, true);
            Outcome result;
            try {
                result = workspace.apply(Command::NewTab);
            }
            catch (...) {
                nextSettings = false;
                nextSettingsStartedAt = 0;
                if (perfLog.enabled())
                    perfLog.phase(
                        "settings_factory", perfClockMs() - startedAt, false);
                throw;
            }
            nextSettings = false;
            nextSettingsStartedAt = 0;
            if (perfLog.enabled())
                perfLog.phase("settings_factory",
                              perfClockMs() - startedAt,
                              result != Outcome::Ignored);
            if (result == Outcome::Ignored)
                notify("no room for a settings tab");
            else
                workspace.setUserTitle(workspace.activeIndex(), "Settings");
        }
        else if (id == "settings.reload") {
            const auto event = settingsWatch.reloadNow();
            reportSettings(event, true);
            if (event == settings::SettingsWatcher::Event::None &&
                settingsWatch.error().empty() &&
                settingsWatch.warnings().empty()) {
                notify(""); // Read again, nothing to say.
            }
        }
        else if (id == "server.install.y" || id == "server.install.n") {
            for (auto &server : serverWindows) {
                if (server.process.pid != 0 && argument == std::to_string(server.process.pid) &&
                    installDialogPid == server.process.pid) {
                    if (!frontend::answerServerConfirmation(server.process.statusFd,
                                                            id == "server.install.y")) {
                        ::close(server.process.statusFd);
                        server.process.statusFd = -1; // EOF denies the copy.
                    }
                    else {
                        serverStatus(server.pane, id == "server.install.y"
                                                      ? "Copy approved; installing on remote host...\n"
                                                      : "Copy declined.\n");
                    }
                    server.installQuestion.clear();
                    installDialogPid.reset();
                    break;
                }
            }
        }
        else if (id == "server.connect" && colon == std::string::npos) {
            std::vector<ui::PickList::Item> items;
            for (const auto &host : settingsWatch.settings().serverHosts()) {
                items.push_back({host,
                                 "SSH daemon in a new window",
                                 "server.connect.go:" + host});
            }
            items.push_back({"New host...",
                             "Enter an SSH destination",
                             "server.connect.new"});
            wv.pushLayer(std::make_unique<ui::PickList>("Connect to server",
                                                        std::move(items)));
        }
        else if (id == "server.connect.new" && colon == std::string::npos) {
            wv.pushLayer(std::make_unique<ui::TextPrompt>(
                "SSH host (user@host)", "", "server.connect.new:", false, 128));
        }
        else if (id == "server.connect.new") {
            if (!safeSshHost(argument)) {
                notify("Invalid SSH host; use a host or user@host without "
                       "spaces or options");
            }
            else {
                wv.pushLayer(std::make_unique<ui::ConfirmDialog>(
                    "Save " + argument + " in Settings?",
                    std::vector<ui::ConfirmDialog::Button>{
                        {"Save and connect", "server.connect.save:" + argument},
                        {"Connect once", "server.connect.go:" + argument},
                        {"Cancel", ""}},
                    2,
                    0));
            }
        }
        else if (id == "server.connect.save") {
            if (settingsWatch.reloadNow() ==
                    settings::SettingsWatcher::Event::Failed ||
                !settingsWatch.error().empty()) {
                notify("Cannot save server: " + settingsWatch.error());
            }
            else {
                auto changed = settingsWatch.settings();
                std::string error;
                if (!changed.addServerHost(argument, error) ||
                    !settingsWatch.save(changed, error)) {
                    notify("Cannot save server: " + error);
                }
                else {
                    startServerWindow(argument);
                }
            }
        }
        else if (id == "server.connect.go") {
            startServerWindow(argument);
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
                wv.pushLayer(stopDaemonDialog(
                    daemonStopCount, frontend::daemonProgramNames(daemonPath)));
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
                wv.pushLayer(stopDaemonDialog(
                    daemonStopCount, frontend::daemonProgramNames(daemonPath)));
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
            else if (found->thisTerminal) {
                notify(thisTerminalNotice());
            }
            else if (found->inThisWindow || !here || !here->daemon) {
                notify("this window already shows that terminal");
            }
            else if (found->attached && !force) {
                wv.pushLayer(takeOverDialog(sessionId, shortFolder(found->cwd), daemonaction::attachHereGo));
            }
            else if (emptyShellNow(true).pane != 0) {
                stageReplacement(sessionId, true);
            }
            else {
                const auto state = paneState(*here);
                wv.pushLayer(replaceTerminalDialog(
                    sessionId + (force ? ":force" : ""),
                    state.busy ? commandLine(state.command) : std::string()));
            }
        }
        else if (id == daemonaction::attachHereDo) {
            const auto colon2 = argument.rfind(':');
            std::string sessionId = colon2 == std::string::npos
                                        ? argument
                                        : argument.substr(0, colon2);
            const bool endOld = colon2 == std::string::npos || argument.substr(colon2 + 1) == "end";
            const bool force =
                sessionId.size() > 6 &&
                sessionId.compare(sessionId.size() - 6, 6, ":force") == 0;
            if (force)
                sessionId.resize(sessionId.size() - 6);
            const auto sessions = daemonSessions(false);
            const auto found = std::find_if(
                sessions.begin(), sessions.end(), [&](const auto &entry) {
                    return entry.id == sessionId;
                });
            if (found == sessions.end())
                notify(
                    "that terminal is gone; the current terminal is unchanged");
            else if (found->thisTerminal || found->inThisWindow)
                notify("that terminal cannot replace this pane");
            else if (found->attached && !force)
                wv.pushLayer(takeOverDialog(sessionId,
                                            shortFolder(found->cwd),
                                            daemonaction::attachHereGo));
            else
                stageReplacement(sessionId, endOld);
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
                    notify("tab detached as layout '" + name + "' (Attach tab... opens it again)");
                }
            }
        }
        else if (id == daemonaction::attachLayout && colon == std::string::npos) {
            std::vector<ui::PickList::Item> items;
            for (const auto &info : recovery.files().list()) {
                if (!recovery.available(info.name))
                    continue;
                const auto saved = recovery.files().load(info.name);
                if (saved && !saved->tabs.empty())
                    items.push_back({saved->tabs[0].title,
                                     "Recover named tab",
                                     std::string(daemonaction::attachLayout) +
                                         ":recovery/" + info.name});
            }
            for (const auto &info : layoutFiles.list()) {
                items.push_back(
                    {info.name,
                     std::to_string(info.panes) +
                         (info.panes == 1 ? " terminal" : " terminals"),
                     std::string(daemonaction::attachLayout) + ":" +
                         info.name});
            }
            wv.pushLayer(std::make_unique<ui::PickList>(
                "Attach tab", std::move(items), "No detached tabs"));
        }
        else if (id == daemonaction::attachLayout ||
                 id == daemonaction::attachLayoutGo) {
            const bool force =
                id == daemonaction::attachLayoutGo && argument.size() > 6 &&
                argument.compare(argument.size() - 6, 6, ":force") == 0;
            const std::string layoutName =
                force ? argument.substr(0, argument.size() - 6) : argument;
            const bool recovering = layoutName.compare(0, 9, "recovery/") == 0;
            const std::string storedName =
                recovering ? layoutName.substr(9) : layoutName;
            const SessionFiles &source =
                recovering ? recovery.files() : layoutFiles;
            const auto snapshot =
                SessionSnapshot::validName(storedName) &&
                        (!recovering || recovery.available(storedName))
                    ? source.load(storedName)
                    : std::nullopt;
            if (!snapshot || snapshot->tabs.empty()) {
                notify("layout '" + layoutName + "' cannot be read");
            }
            else {
                const auto plan = planLayoutAttach(snapshot->tabs[0], daemonSessions(false));
                if (recovering && (plan.takeOver > 0 || plan.fresh > 0)) {
                    notify("Recovery refused: terminals are missing or attached elsewhere; no new shells started");
                }
                else if (plan.takeOver > 0 && !force) {
                    wv.pushLayer(takeOverLayoutDialog(layoutName, plan.takeOver));
                }
                else {
                    const EmptyShell empty = emptyShellNow();
                    // Panes are made in the restore's own order, so a terminal is found by its id.
                    std::set<std::string> usable;
                    for (const auto &u : plan.use) {
                        if (!u.empty()) usable.insert(u);
                    }
                    struct RecoveryGuard {
                        bool &flag;
                        std::string &attach, &cwd;
                        ~RecoveryGuard() {
                            flag = false;
                            attach.clear();
                            cwd.clear();
                        }
                    } recoveryGuard{recoveryAttachOnly, nextAttachId, nextCwd};
                    recoveryAttachOnly = recovering;
                    const auto report = restoreSession(
                        workspace, *snapshot, [&](const SavedPane &saved) {
                            nextAttachId.clear();
                            if (!saved.terminal.empty() &&
                                usable.erase(saved.terminal) != 0) {
                                nextAttachId =
                                    saved.terminal; // Once: a terminal named
                                                    // twice is shown once.
                            }
                            std::error_code ec;
                            nextCwd =
                                cwdToRequest(saved.cwd,
                                             remoteDaemon,
                                             !saved.cwd.empty() &&
                                                 std::filesystem::is_directory(
                                                     saved.cwd, ec));
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
                        if (!recovering || report.refused == 0)
                            source.remove(storedName); // Keep incomplete recoveries for another attempt.
                        const bool closed = closeEmptyShell(empty);
                        notify("layout '" + layoutName + "' attached" + (closed ? "; empty shell closed" : ""));
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
            else if (found->thisTerminal) {
                notify(thisTerminalNotice());
            }
            else if (found->inThisWindow) {
                notify("this window already shows that terminal");
            }
            else if (found->attached && !force) {
                wv.pushLayer(takeOverDialog(sessionId, shortFolder(found->cwd)));
            }
            else {
                const EmptyShell empty = emptyShellNow();
                if (attachSession(sessionId) && closeEmptyShell(empty)) {
                    notify("empty shell closed");
                }
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
            const EmptyShell empty = emptyShellNow();
            bool any = false;
            for (const auto &s : daemonSessions(false)) {
                if (!s.attached && !s.inThisWindow) {
                    if (!attachSession(s.id)) {
                        break;
                    }
                    any = true;
                }
            }
            if (any && closeEmptyShell(empty)) {
                notify("empty shell closed");
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
            bool allIdentified = true;
            auto snapshot = captureSession(
                workspace, "layout", std::int64_t(std::time(nullptr)), [&](PaneId, const NativePane &pane) {
                    SavedPane saved;
                    const auto state = paneState(const_cast<NativePane &>(pane));
                    saved.cwd = state.cwd;
                    saved.command = state.command;
                    if (pane.daemon && LayoutSnapshot::validId(pane.daemon->sessionId()))
                        saved.terminal = pane.daemon->sessionId();
                    else
                        allIdentified = false;
                    return saved;
                });
            if (!snapshot || !allIdentified) {
                notify("layouts not saved: a terminal has no reusable daemon session");
                return;
            }
            for (auto &tab : snapshot->tabs) {
                const std::string base = tab.title.empty() ? "layout" : SessionPolicy("layout").suggestion(tab.title);
                std::string name;
                for (int n = 1; n <= 9999; ++n) {
                    const std::string suffix = n == 1 ? "" : "-" + std::to_string(n);
                    const std::string candidate = base.substr(0, SessionSnapshot::kMaxName - suffix.size()) + suffix;
                    std::error_code ec;
                    const auto status = std::filesystem::symlink_status(
                        std::filesystem::path(layoutFiles.directory()) / (candidate + ".json"), ec);
                    if (ec && ec != std::errc::no_such_file_or_directory) {
                        notify("layouts not saved: " + ec.message());
                        return;
                    }
                    if (status.type() == std::filesystem::file_type::not_found) {
                        name = candidate;
                        break;
                    }
                }
                if (name.empty()) {
                    notify("layouts not saved: no free layout name");
                    return;
                }
                SessionSnapshot one;
                one.name = name;
                one.saved = snapshot->saved;
                one.active = 0;
                one.tabs.push_back(std::move(tab));
                std::string error;
                if (!layoutFiles.save(one, &error)) {
                    notify("layouts not saved: " + error + "; window remains open");
                    return;
                }
            }
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
    const auto paletteEntryNamed = [](const std::vector<CommandEntry> &entries, const char *name) {
        for (const auto &entry : entries) {
            if (entry.name == name) {
                return true;
            }
        }
        return false;
    };
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
    // --pty-layout-smoke-test: Detach tab, then the palette offers Attach tab... (docs/tasks/daemon-versioning.md).
    std::string smokeAttachId;
    std::size_t smokeCount = 0;
    std::size_t smokeTabsBefore = 1;
    PaneId smokeOtherPane = 0, smokeDetachedPane = 0;
    std::string smokeOriginalSession;
    NativePane *smokeOriginalObject = nullptr;
    std::unique_ptr<NativePane> smokePeer;
    const auto layoutScript = [&] {
        const auto later = [&](Uint64 ms) { scriptDue = SDL_GetTicks64() + ms; };
        const auto connected = [](const NativePane *p) { return p && p->daemon && !p->daemon->sessionId().empty(); };
        // MATTERM_LAYOUT_SMOKE: "detach-only" detaches the only tab (the window ends), "check" is a later
        // window that must offer the layout; otherwise both in one window.
        const char *variantText = std::getenv("MATTERM_LAYOUT_SMOKE");
        const std::string variant = variantText ? variantText : "";
        if (variant == "recovery-create" || variant == "recovery-check") {
            if (!connected(workspace.focusedPane()))
                return;
            if (variant == "recovery-create") {
                if (scriptStep == 0) {
                    workspace.setUserTitle(workspace.activeIndex(),
                                           "crash-work");
                    workspace.apply(Command::SplitSideBySide);
                    ++scriptStep;
                }
                return; // Parent test SIGKILLs only this window after the
                        // checkpoint.
            }
            const auto checkpoints = recovery.files().list();
            expectThat(checkpoints.size() == 1,
                       "one named tab checkpoint survived");
            const auto saved = recovery.files().load(checkpoints.front().name);
            expectThat(saved && recovery.available(checkpoints.front().name),
                       "dead window recovery is available");
            const auto plan =
                planLayoutAttach(saved->tabs[0], daemonSessions(false));
            expectThat(plan.reused == 2 && plan.fresh == 0 &&
                           plan.takeOver == 0,
                       "both original daemon terminals survived SIGKILL");
            handleAction(std::string(daemonaction::attachLayout) +
                         ":recovery/" + checkpoints.front().name);
            expectThat(workspace.userTitle(workspace.activeIndex()) ==
                           "crash-work",
                       "recovered tab name");
            expectThat(activeTabPanes().size() == 2, "recovered split layout");
            scriptDone = true;
            keepTerminals = true;
            running = false;
            return;
        }
        switch (scriptStep++) {
        case 0:
            later(2000); // The tunnel, the daemon and the first terminal.
            break;
        case 1:
            expectThat(connected(workspace.focusedPane()),
                       "the first terminal is attached to the daemon");
            if (variant.compare(0, 11, "attachhere-") == 0) {
                workspace.apply(Command::SplitSideBySide);
                scriptStep = 160;
                later(1500);
            }
            else if (variant == "pane-detach" || variant == "tab-detach") {
                smokeOtherPane = workspace.focusedPaneId();
                workspace.apply(Command::NewTab);
                workspace.apply(Command::SplitSideBySide);
                scriptStep = 140;
                later(2000);
            }
            else if (variant == "window-detach") {
                expectThat(workspace.apply(Command::NewTab) != Outcome::Ignored,
                           "a second daemon tab opens for window detach");
                scriptStep = 120;
                later(2000);
            }
            else if (variant == "window-reattach") {
                const char *text = std::getenv("MATTERM_DETACH_IDS");
                std::string ids = text ? text : "";
                expectThat(!ids.empty(),
                           "the earlier window supplied retained terminal IDs");
                smokeCount =
                    std::size_t(std::count(ids.begin(), ids.end(), ',')) + 1;
                if (const char *layout =
                        std::getenv("MATTERM_REATTACH_LAYOUT")) {
                    handleAction(std::string(daemonaction::attachLayout) + ":" +
                                 layout);
                }
                else {
                    std::size_t begin = 0;
                    do {
                        const auto comma = ids.find(',', begin);
                        expectThat(
                            attachSession(ids.substr(begin,
                                                     comma == std::string::npos
                                                         ? comma
                                                         : comma - begin)),
                            "reattach a retained terminal");
                        if (comma == std::string::npos)
                            break;
                        begin = comma + 1;
                    } while (begin < ids.size());
                }
                scriptStep = 122;
                later(2000);
            }
            else if (variant == "settings" || variant == "settings-palette") {
                if (variant == "settings-palette") {
                    workspace.apply(Command::SplitSideBySide);
                    workspace.apply(Command::NewTab);
                    workspace.apply(Command::SplitStacked);
                    workspace.apply(Command::NewTab);
                    workspace.apply(Command::SplitSideBySide);
                    smokeTabsBefore = workspace.tabCount();
                }
                expectThat(paletteEntryNamed(paletteExtra(), "Settings"),
                           "the palette offers Settings");
                std::string error;
                const auto reply =
                    askDaemon(daemonPath, mtd::ListSessions{}, error, 2000);
                if (const auto *list =
                        reply ? std::get_if<mtd::SessionList>(&*reply)
                              : nullptr)
                    smokeCount = list->sessions.size();
                failNextSettingsForTest = true;
                bool rejected = false;
                try {
                    handleAction("settings.open");
                }
                catch (const std::runtime_error &) {
                    rejected = true;
                }
                expectThat(
                    rejected && !nextSettings,
                    "a failed settings factory clears its local-only override");
                if (variant == "settings-palette") {
                    openPalette();
                    SDL_Event search{};
                    search.type = SDL_TEXTINPUT;
                    SDL_strlcpy(
                        search.text.text, "Settings", sizeof search.text.text);
                    SDL_PushEvent(&search);
                    pushKey(SDLK_RETURN, KMOD_NONE);
                }
                else {
                    handleAction("settings.open");
                }
                later(1500);
                scriptStep = 110;
            }
            else if (variant == "emptyshell" || variant == "emptyshell-typed") {
                // A terminal that no window shows, and the fresh shell in front: Attach terminal... closes
                // the fresh shell only when nothing was typed in it.
                std::string error;
                const auto reply = askDaemon(daemonPath, mtd::CreateSession{"", 80, 24}, error, 2000);
                const auto *created = reply ? std::get_if<mtd::SessionCreated>(&*reply) : nullptr;
                expectThat(created != nullptr, "the daemon makes a second terminal");
                smokeAttachId = created->id;
                if (variant == "emptyshell-typed") {
                    expectThat(workspace.focusedPane()->v.sendInput("x"), "something is typed into the first shell");
                }
                later(1500); // The typed byte is sent and the shell has shown its prompt.
                scriptStep = 100;
            }
            else if (variant == "programs" || variant == "programs-skip") {
                handleAction("session.open:progtest");
                expectThat(wv.hasLayer() && pendingPrograms.size() == 2, "Open session asks once about the two programs");
                handleAction(variant == "programs" ? restoreaction::start : restoreaction::skip);
                later(2500);
            }
            else if (variant == "cwd") {
                // A saved session whose folders are known to the daemon's computer: one exists, one does not.
                handleAction("session.open:cwdtest");
                later(2500);
            }
            else if (variant == "check") {
                expectThat(layoutFiles.list().size() == 1, "the layout of the earlier window is still there");
                expectThat(paletteEntryNamed(paletteExtra(), "Attach tab..."), "the palette offers Attach tab...");
                scriptDone = true;
                running = false;
            }
            else if (variant == "detach-only") {
                scriptDone = true; // The window ends with its only tab.
                handleAction(daemonaction::detachTab);
                expectThat(layoutFiles.list().size() == 1, "Detach tab saves the layout");
            }
            else {
                expectThat(workspace.apply(Command::NewTab) != Outcome::Ignored, "a second tab opens");
                later(2000);
            }
            break;
        case 160: {
            smokeDetachedPane = workspace.focusedPaneId();
            smokeOriginalObject = workspace.focusedPane();
            smokeOriginalSession = smokeOriginalObject->daemon->sessionId();
            mtd::CreateTerminal request;
            request.command = {"sh", "-c", "echo ATTACH-HERE-TARGET; sleep 60"};
            std::string error;
            const auto target = createTerminal(daemonPath, request, error);
            expectThat(target.has_value(),
                       "create an independent attach-here target");
            smokeAttachId = *target;
            if (variant == "attachhere-typed")
                smokeOriginalObject->v.sendInput("x\x7f");
            if (variant == "attachhere-busy")
                smokeOriginalObject->v.sendInput("sleep 60\n");
            if (variant == "attachhere-takeover") {
                smokePeer = std::make_unique<NativePane>(
                    daemonPath, smokeAttachId, "", 80, 24);
                smokePeer->endInDaemon = false;
            }
            scriptStep = 161;
            later(1500);
            break;
        }
        case 161: {
            if (smokePeer)
                smokePeer->pump();
            if (variant == "attachhere-fail") {
                smokeOriginalObject->v.selection =
                    Selection::all(smokeOriginalObject->v.model());
                stageReplacement("nonexistent-test-terminal", true);
            }
            else if (variant == "attachhere-cancel" ||
                     variant == "attachhere-timeout" ||
                     variant == "attachhere-stale" ||
                     variant == "attachhere-closed") {
                stageReplacement(smokeAttachId, true);
                if (variant == "attachhere-cancel")
                    pushKey(SDLK_ESCAPE, KMOD_NONE); // Real event path cancels
                                                     // before the next pump.
                else if (variant == "attachhere-timeout")
                    pendingReplacements.back().deadline =
                        std::int64_t(SDL_GetTicks64());
            }
            else {
                failEmptyProbeForTest = variant == "attachhere-unknown";
                handleAction(std::string(daemonaction::attachHereGo) + ":" +
                             smokeAttachId);
                failEmptyProbeForTest = false;
                if (variant == "attachhere-typed" ||
                    variant == "attachhere-busy" ||
                    variant == "attachhere-unknown") {
                    expectThat(wv.hasLayer() && pendingReplacements.empty(),
                               "typed/deleted or busy panes still ask before "
                               "replacement");
                    afterLayout(wv.key({keys::Escape, false, false, false},
                                       std::int64_t(SDL_GetTicks64())));
                }
                else if (variant == "attachhere-takeover") {
                    expectThat(wv.hasLayer() && pendingReplacements.empty(),
                               "even an untouched pane requires confirmation "
                               "before taking another client's terminal");
                    wv.closeLayers();
                    handleAction(std::string(daemonaction::attachHereGo) + ":" +
                                 smokeAttachId + ":force");
                    expectThat(pendingReplacements.size() == 1,
                               "confirmed takeover can stage an unused split "
                               "without a redundant replace question");
                }
                else {
                    expectThat(!wv.hasLayer() &&
                                   pendingReplacements.size() == 1,
                               "unused split bypasses Replace but preserves "
                               "the old owner while attaching");
                }
            }
            expectThat(workspace.pane(smokeDetachedPane) ==
                               smokeOriginalObject &&
                           smokeOriginalObject->daemon->sessionId() ==
                               smokeOriginalSession,
                       "starting an attachment never destroys the original "
                       "synchronously");
            if (variant == "attachhere-stale") {
                auto newer = std::make_unique<NativePane>(
                    daemonPath, smokeOriginalSession, "", 80, 24);
                auto old =
                    workspace.replacePaneAnywhere(smokeDetachedPane, newer);
                old->endInDaemon = false;
                old.reset();
                smokeOriginalObject = workspace.pane(smokeDetachedPane);
            }
            else if (variant == "attachhere-closed") {
                smokeOriginalObject->endInDaemon = false;
                workspace.closePaneAnywhere(smokeDetachedPane);
            }
            scriptStep = 162;
            later(1500);
            break;
        }
        case 162: {
            if (variant == "attachhere-closed") {
                expectThat(!workspace.pane(smokeDetachedPane) &&
                               workspace.paneCount() == 1 &&
                               pendingReplacements.empty(),
                           "closing the original pane aborts its pending "
                           "attach without affecting another split");
                expectThat(
                    daemonSessions().size() == 3,
                    "stale closed-target staging ends neither daemon terminal");
                scriptDone = true;
                keepTerminals = true;
                running = false;
                break;
            }
            const bool success = variant == "attachhere-empty" ||
                                 variant == "attachhere-takeover";
            auto *pane = workspace.pane(smokeDetachedPane);
            expectThat(pane && workspace.paneCount() == 2 &&
                           workspace.tabCount() == 1 &&
                           workspace.focusedPaneId() == smokeDetachedPane,
                       "replacement keeps the pane ID, split count and focus");
            expectThat(pendingReplacements.empty(),
                       "staging finished or aborted within its bound");
            const auto sessions = daemonSessions();
            if (success) {
                expectThat(pane != smokeOriginalObject &&
                               pane->daemon->sessionId() == smokeAttachId &&
                               !pane->v.ended(),
                           "a verified live new attachment atomically replaces "
                           "the original owner");
                expectThat(
                    visibleText(pane->v.model()).find("ATTACH-HERE-TARGET") !=
                        std::string::npos,
                    "the attached program replays in the same split");
                expectThat(
                    sessions.size() == 2,
                    "the old terminal ends only after successful attachment");
            }
            else {
                expectThat(pane == smokeOriginalObject &&
                               pane->daemon->sessionId() ==
                                   smokeOriginalSession &&
                               !pane->v.ended(),
                           "failure, timeout, cancel and safe refusal preserve "
                           "the original terminal");
                expectThat(
                    sessions.size() == 3,
                    "failure/refusal does not end either daemon terminal");
                if (variant == "attachhere-fail")
                    expectThat(
                        pane->v.selection.has_value(),
                        "failed attachment retains the original selection");
            }
            smokePeer.reset();
            scriptDone = true;
            keepTerminals = true;
            running = false;
            break;
        }
        case 140: {
            expectThat(workspace.tabCount() == 2 && workspace.paneCount() == 3,
                       "scoped detach starts with an unrelated tab and two "
                       "target splits");
            smokeDetachedPane = workspace.focusedPaneId();
            std::string ids;
            for (const auto paneId : workspace.activeTab()->tree.ids()) {
                if (variant == "pane-detach" && paneId != smokeDetachedPane)
                    continue;
                if (!ids.empty())
                    ids += ',';
                ids += workspace.pane(paneId)->daemon->sessionId();
            }
            std::printf("window-detach-ids:%s\n", ids.c_str());
            workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                expectThat(
                    pane.v.sendInput("echo WINDOW-DETACH-ALIVE; sleep 60\n"),
                    "start a busy program for scoped detach");
            });
            scriptStep = 141;
            later(2000);
            break;
        }
        case 141: {
            const bool tab = variant == "tab-detach";
            const auto ask = [&] {
                afterLayout(wv.key({'w', tab, true, !tab},
                                   std::int64_t(SDL_GetTicks64())));
                expectThat(
                    wv.pendingClose() &&
                        wv.pendingClose()->scope ==
                            (tab ? CloseScope::Tab : CloseScope::Pane),
                    "the scoped close key asks before ending busy programs");
                expectThat(canDetachClose(*wv.pendingClose()),
                           "all live target panes can survive detach");
            };
            ask();
            afterLayout(wv.key({keys::Escape, false, false, false},
                               std::int64_t(SDL_GetTicks64())));
            expectThat(
                !wv.pendingClose() && workspace.tabCount() == 2 &&
                    workspace.paneCount() == 3,
                "Escape cancels scoped close without changing tabs or panes");
            ask();
            workspace.focusPane(smokeOtherPane); // Confirmation must retain its
                                                 // original target.
            wv.closeLayers();
            handleAction("close.detach");
            expectThat(running && workspace.pane(smokeOtherPane) &&
                           workspace.tabCount() == (tab ? 1u : 2u),
                       "scoped detach preserves the unrelated tab and window");
            expectThat(!workspace.pane(smokeDetachedPane) &&
                           workspace.paneCount() == (tab ? 1u : 2u),
                       "only the requested pane or tab leaves the workspace");
            expectThat(daemonSessions().size() == 3,
                       "scoped detach sends no terminal-ending request");
            if (tab) {
                const auto layouts = layoutFiles.list();
                expectThat(layouts.size() == 1 && layouts.front().panes == 2,
                           "tab detach saves both splits as a reusable layout");
                std::printf("scoped-detach-layout:%s\n",
                            layouts.front().name.c_str());
            }
            else {
                expectThat(layoutFiles.list().empty(),
                           "pane detach does not create a tab layout");
            }
            scriptDone = true;
            keepTerminals = true;
            running = false;
            break;
        }
        case 120: {
            std::string ids;
            workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                expectThat(connected(&pane),
                           "every window pane has a daemon terminal");
                if (!ids.empty())
                    ids += ',';
                ids += pane.daemon->sessionId();
                expectThat(
                    pane.v.sendInput("echo WINDOW-DETACH-ALIVE; sleep 60\n"),
                    "start a long running program in each tab");
            });
            std::printf("window-detach-ids:%s\n", ids.c_str());
            scriptStep = 121;
            later(2000);
            break;
        }
        case 121: {
            const auto before = daemonSessions();
            requestQuit();
            const auto pending = wv.pendingClose();
            expectThat(pending && pending->scope == CloseScope::Window &&
                           canDetachWindow(),
                       "a busy daemon window asks and permits detach");
            handleAction("close.cancel");
            expectThat(running && !wv.pendingClose(),
                       "Cancel keeps the whole window alive");
            const auto after = daemonSessions();
            expectThat(before.size() == 2 && after.size() == before.size() &&
                           std::all_of(before.begin(),
                                       before.end(),
                                       [&](const auto &entry) {
                                           return std::any_of(
                                               after.begin(),
                                               after.end(),
                                               [&](const auto &row) {
                                                   return row.id == entry.id;
                                               });
                                       }),
                       "Cancel leaves both daemon session IDs untouched");
            requestQuit();
            handleAction("close.detach");
            expectThat(!running && keepTerminals,
                       "Window Detach selects the existing keep-terminals "
                       "shutdown policy");
            const auto layouts = layoutFiles.list();
            expectThat(layouts.size() == 2, "Window Detach saves both tab layouts");
            scriptDone = true;
            break;
        }
        case 122: {
            int retained = 0;
            workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                if (visibleText(pane.v.model()).find("WINDOW-DETACH-ALIVE") !=
                    std::string::npos) {
                    ++retained;
                    expectThat(!pane.v.ended() && paneState(pane).busy,
                               "the retained program is still live in the "
                               "daemon, not just stale replay");
                }
            });
            expectThat(retained == int(smokeCount),
                       "all scoped programs survived detach and replay after "
                       "reattachment");
            if (std::getenv("MATTERM_REATTACH_LAYOUT"))
                expectThat(workspace.activeTab()->tree.size() == smokeCount,
                           "tab reattachment restores the saved split layout");
            scriptDone = true;
            running = false;
            break;
        }
        case 110: {
            NativePane *pane = workspace.focusedPane();
            const bool local = pane && pane->link && !pane->daemon;
            expectThat(workspace.tabCount() == smokeTabsBefore + 1 && local,
                       "Settings opens a LOCAL tab, also with --daemon-host");
            if (!local) {
                scriptDone = true;
                running = false;
                break;
            }
            expectThat(visibleText(pane->v.model()).find("Settings") !=
                           std::string::npos,
                       "the local TUI has drawn");
            expectThat(
                !canDetachWindow(),
                "a live local Settings PTY prevents whole-window detach");
            CloseRequest localPane;
            localPane.scope = CloseScope::Pane;
            localPane.pane = workspace.focusedPaneId();
            expectThat(!canDetachClose(localPane),
                       "a local Settings pane does not offer Detach");
            CloseRequest localTab;
            localTab.scope = CloseScope::Tab;
            localTab.tab = workspace.activeTab()->id;
            expectThat(!canDetachClose(localTab),
                       "a tab containing local Settings does not offer Detach");
            localTab.tab = workspace.tab(0)->id;
            expectThat(canDetachClose(localTab),
                       "an unrelated daemon-only tab remains eligible despite "
                       "local Settings elsewhere");
            requestQuit();
            expectThat(wv.pendingClose().has_value(),
                       "the mixed window asks before closing");
            handleAction("close.detach");
            wv.closeLayers(); // The direct test action bypassed the dialog's
                              // normal pop.
            expectThat(running && !keepTerminals && !pane->v.ended(),
                       "even a stale detach action cannot kill a mixed "
                       "local/daemon window");
            expectThat(pane->v.sendInput("theme\r\x1b[B\r"),
                       "choose Blue in the local settings program");
            later(2000);
            scriptStep = 111;
            break;
        }
        case 111: {
            expectThat(settingsWatch.settings().choice("theme") == "blue",
                       "the native watcher follows the locally saved setting");
            std::string error;
            const auto reply =
                askDaemon(daemonPath, mtd::ListSessions{}, error, 2000);
            const auto *list =
                reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr;
            expectThat(list && list->sessions.size() == smokeCount,
                       "Settings did not create a daemon terminal");
            expectThat(settingsWatch.settings().flag("drawBatching"),
                       "draw batching defaults on in the native client");
            if (auto *pane = workspace.focusedPane())
                pane->v.sendInput("\x1b\x1b"
                                  "Draw batching\r\x1b[B\r");
            later(2000);
            scriptStep = 112;
            break;
        }
        case 112: {
            expectThat(!settingsWatch.settings().flag("drawBatching"),
                       "the native client follows locally disabled draw "
                       "batching live, also over SSH");
            expectThat(
                !holdFloodDraw(settingsWatch.settings().flag("drawBatching"),
                               true,
                               false,
                               10),
                "off bypasses only the draw hold policy during a flood");
            if (auto *pane = workspace.focusedPane())
                pane->v.sendInput("\x11");
            if (variant == "settings-palette") {
                scriptStep = 113;
                later(1000);
            }
            else {
                scriptDone = true;
                running = false;
            }
            break;
        }
        case 113: {
            expectThat(
                workspace.tabCount() == smokeTabsBefore,
                "closing Settings through its own quit leaves the other tabs");
            openPalette();
            SDL_Event search{};
            search.type = SDL_TEXTINPUT;
            SDL_strlcpy(search.text.text, "Settings", sizeof search.text.text);
            SDL_PushEvent(&search);
            pushKey(SDLK_RETURN, KMOD_NONE);
            scriptStep = 114;
            later(1500);
            break;
        }
        case 114: {
            auto *pane = workspace.focusedPane();
            expectThat(workspace.tabCount() == smokeTabsBefore + 1 && pane &&
                           pane->link && !pane->daemon,
                       "Settings reopens from the palette in the same multitab "
                       "process");
            expectThat(visibleText(pane->v.model()).find("Settings") !=
                           std::string::npos,
                       "the reopened local Settings TUI has drawn");
            pane->v.sendInput("theme\r\x1b[B\r");
            scriptStep = 115;
            later(1500);
            break;
        }
        case 115: {
            expectThat(settingsWatch.settings().choice("theme") == "green",
                       "the reopened Settings process can save another change");
            afterLayout(wv.key({'w', true, true, false},
                               std::int64_t(SDL_GetTicks64())));
            expectThat(wv.pendingClose() &&
                           wv.pendingClose()->scope == CloseScope::Tab,
                       "closing the local Settings tab asks before killing its "
                       "process");
            wv.closeLayers();
            handleAction("close.confirm");
            expectThat(
                running && workspace.tabCount() == smokeTabsBefore,
                "confirmed local Settings close preserves the other tabs");
            scriptStep = 116;
            later(500);
            break;
        }
        case 116: {
            openPalette();
            SDL_Event search{};
            search.type = SDL_TEXTINPUT;
            SDL_strlcpy(search.text.text, "Settings", sizeof search.text.text);
            SDL_PushEvent(&search);
            pushKey(SDLK_RETURN, KMOD_NONE);
            scriptStep = 117;
            later(1500);
            break;
        }
        case 117: {
            auto *pane = workspace.focusedPane();
            expectThat(workspace.tabCount() == smokeTabsBefore + 1 && pane &&
                           pane->link && !pane->daemon && !pane->v.ended(),
                       "Settings reopens again after confirmed tab close "
                       "without terminating the native window");
            pane->v.sendInput("\x11");
            scriptDone = true;
            running = false;
            break;
        }
        case 100: {
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::ListSessions{}, error, 2000);
            if (const auto *list = reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr) {
                smokeCount = list->sessions.size(); // Earlier variants may have left terminals in the daemon.
            }
            handleAction(std::string(daemonaction::attachGo) + ":" + smokeAttachId);
            later(2500);
            scriptStep = 101;
            break;
        }
        case 101: {
            const bool typed = variant == "emptyshell-typed";
            expectThat(workspace.tabCount() == (typed ? 2u : 1u),
                       typed ? "a shell that was typed in stays" : "the empty shell closed after the attach");
            const NativePane *shown = workspace.focusedPane();
            expectThat(shown && shown->daemon && shown->daemon->sessionId() == smokeAttachId, "the attached terminal is in front");
            std::size_t terminals = 0;
            std::string error;
            const auto reply = askDaemon(daemonPath, mtd::ListSessions{}, error, 2000);
            if (const auto *list = reply ? std::get_if<mtd::SessionList>(&*reply) : nullptr) {
                terminals = list->sessions.size();
            }
            expectThat(terminals == (typed ? smokeCount : smokeCount - 1),
                       typed ? "both terminals still run" : "and the fresh terminal ended in the daemon");
            scriptDone = true;
            running = false;
            break;
        }
        case 2:
            if (variant == "programs" || variant == "programs-skip") {
                std::string all;
                workspace.forEachPane([&](PaneId, NativePane &p, bool) { all += visibleText(p.v.model()) + "\n"; });
                if (variant == "programs") {
                    expectThat(all.find("starting again: sleep 1") != std::string::npos, "Start them types the programs");
                    expectThat(all.find("was running") == std::string::npos, "and leaves no prompt-line offers");
                }
                else {
                    expectThat(all.find("was running: sleep 1") != std::string::npos, "Not now leaves the prompt-line offers");
                    expectThat(all.find("starting again") == std::string::npos, "and starts nothing");
                }
                scriptDone = true;
                running = false;
                break;
            }
            if (variant == "cwd") {
                const char *okText = std::getenv("MATTERM_SMOKE_CWD_OK");
                const char *missingText = std::getenv("MATTERM_SMOKE_CWD_MISSING");
                const std::string ok = okText ? okText : "", missing = missingText ? missingText : "";
                std::vector<std::string> folders;
                workspace.forEachPane([&](PaneId, NativePane &p, bool) { folders.push_back(paneState(p).cwd); });
                expectThat(folders.size() == 3, "the session adds its two panes");
                expectThat(std::count(folders.begin(), folders.end(), ok) == 1,
                           "a saved folder is restored (the daemon's computer has it)");
                expectThat(std::count(folders.begin(), folders.end(), missing) == 0,
                           "a folder that is gone is not entered");
                scriptDone = true;
                running = false;
                break;
            }
            expectThat(workspace.tabCount() == 2 && connected(workspace.focusedPane()), "the second terminal is attached");
            expectThat(layoutFiles.list().empty(), "no layout is saved yet");
            expectThat(!paletteEntryNamed(paletteExtra(), "Attach tab..."), "Attach tab... is not offered without a layout");
            handleAction(daemonaction::detachTab);
            later(500);
            break;
        case 3:
            expectThat(workspace.tabCount() == 1, "Detach tab closes the tab");
            expectThat(layoutFiles.list().size() == 1, "Detach tab saves the layout");
            expectThat(paletteEntryNamed(paletteExtra(), "Attach tab..."), "the palette offers Attach tab...");
            scriptDone = true;
            running = false;
            break;
        default: break;
        }
    };
    const auto script = [&] {
        if (!tabsSmoke || scriptDone || SDL_GetTicks64() < scriptDue) {
            return;
        }
        if (daemonOptions.layoutSmoke) {
            layoutScript();
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
    std::uint64_t ingestSeen =
        0; // Output bytes of all panes at the last round (see "flooded").
    Uint64 lastDrawMs = 0;
    bool forceRedraw = true; // The first frame; again after anything that may have disturbed the window.
    std::string shownTitle;
    std::uint64_t framesDrawn = 0, iterations = 0;
    const Uint64 loopStarted = SDL_GetTicks64();
    double previousGuiLoopAt = 0;
    while (running) {
        for (auto &server : serverWindows) {
            if (server.process.statusFd >= 0) {
                char bytes[1024];
                ssize_t n;
                while ((n = ::read(server.process.statusFd,
                                   bytes,
                                   sizeof(bytes))) > 0) {
                    server.incoming.append(bytes, std::size_t(n));
                    if (server.incoming.size() > 8192)
                        server.incoming.clear();
                    std::size_t end;
                    while ((end = server.incoming.find('\n')) !=
                           std::string::npos) {
                        const std::string line = server.incoming.substr(0, end);
                        server.incoming.erase(0, end + 1);
                        if (line == "R") {
                            serverStatus(server.pane,
                                         "Window opened successfully. Closing "
                                         "this tab shortly...\n");
                            server.opened = true;
                            server.readyAt = SDL_GetTicks64();
                        }
                        else if (line.size() > 2 && line[1] == ' ') {
                            if (line[0] == 'P')
                                serverStatus(server.pane,
                                             line.substr(2) + "\n");
                            if (line[0] == 'I') {
                                server.installQuestion = line.substr(2);
                                serverStatus(server.pane,
                                             "Waiting for installation approval: " +
                                                 server.installQuestion + "\n");
                            }
                            if (line[0] == 'E') {
                                serverStatus(server.pane,
                                             "Connection failed: " +
                                                 line.substr(2) + "\n");
                                notify("Connection failed: " + line.substr(2));
                                server.failed = true;
                                server.readyAt = 0;
                            }
                        }
                    }
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    ::close(server.process.statusFd);
                    server.process.statusFd = -1;
                }
            }
            if (!server.exited) {
                int status = 0;
                const pid_t done =
                    ::waitpid(server.process.pid, &status, WNOHANG);
                if (done == server.process.pid ||
                    (done < 0 && errno == ECHILD)) {
                    server.exited = true;
                    if (server.opened && server.readyAt &&
                        (done < 0 || !WIFEXITED(status) ||
                         WEXITSTATUS(status) != 0)) {
                        server.readyAt = 0;
                        server.failed = true;
                        serverStatus(server.pane,
                                     "The new window closed unexpectedly.\n");
                        notify("The new server window closed unexpectedly");
                    }
                    else if (!server.opened && !server.failed &&
                             server.process.statusFd < 0) {
                        serverStatus(
                            server.pane,
                            "The new window exited before it opened.\n");
                        notify("The new window exited before it opened");
                        server.failed = true;
                    }
                }
            }
            if (server.readyAt && SDL_GetTicks64() - server.readyAt >= 1500) {
                if (workspace.pane(server.pane))
                    workspace.closePaneAnywhere(server.pane);
                server.pane = 0;
                server.readyAt = 0;
            }
        }
        serverWindows.erase(std::remove_if(serverWindows.begin(),
                                           serverWindows.end(),
                                           [](const StartingServer &s) {
                                               return s.exited &&
                                                      s.process.statusFd < 0;
                                           }),
                            serverWindows.end());
        if (installDialogPid && !wv.hasLayer()) {
            for (auto &server : serverWindows) {
                if (server.process.pid == *installDialogPid &&
                    server.process.statusFd >= 0) {
                    if (!frontend::answerServerConfirmation(server.process.statusFd, false)) {
                        ::close(server.process.statusFd); // EOF denies the copy.
                        server.process.statusFd = -1;
                    }
                    server.installQuestion.clear();
                    break;
                }
            }
            installDialogPid.reset();
        }
        if (!installDialogPid && !wv.hasLayer()) {
            for (auto &server : serverWindows) {
                if (server.installQuestion.empty() || server.exited ||
                    server.process.statusFd < 0) continue;
                installDialogPid = server.process.pid;
                const auto pid = std::to_string(server.process.pid);
                wv.pushLayer(std::make_unique<ui::ConfirmDialog>(
                    server.installQuestion,
                    std::vector<ui::ConfirmDialog::Button>{
                        {"Copy and start", "server.install.y:" + pid},
                        {"Cancel", "server.install.n:" + pid}},
                    1, 1)); // Explicit consent: Enter and Escape both decline.
                break;
            }
        }
        if (perfLog.enabled()) {
            const double loopAt = perfClockMs();
            perfLog.tick();
            if (previousGuiLoopAt != 0)
                perfLog.second.guiLoopGap.add(loopAt - previousGuiLoopAt);
            previousGuiLoopAt = loopAt;
            perfLog.second.clients = 0;
            workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
                pane.measure(perfLog.second);
            });
        }
        ++iterations;
        if (daemonOptions.enabled &&
            SDL_GetTicks64() - recoveryPollAt >= 1000) {
            recoveryPollAt = SDL_GetTicks64();
            auto captured = captureSession(
                workspace, "checkpoint", 0, [](PaneId, const NativePane &pane) {
                    SavedPane saved;
                    if (pane.daemon && !pane.v.ended())
                        saved.terminal = pane.daemon->sessionId();
                    return saved;
                });
            if (captured) {
                std::set<std::string> current;
                for (std::size_t i = 0; i < captured->tabs.size(); ++i) {
                    auto &tab = captured->tabs[i];
                    if (tab.title.empty() ||
                        !frontend::recoverableNode(*tab.root))
                        continue;
                    std::string error;
                    if (!recovery.checkpoint(workspace.tab(i)->id,
                                             std::move(tab),
                                             current,
                                             error) &&
                        !recoveryWriteFailed) {
                        notify("Cannot save crash recovery: " + error);
                        recoveryWriteFailed = true;
                    }
                }
                recovery.retireExcept(current);
            }
        }
        skipText = false;
        reportSettings(settingsWatch.poll(), false);
        workspace.forEachPane(
            [&](PaneId,
                NativePane &pane,
                bool) { // Panes made since, and a theme that changed.
                const auto &now = pane.v.palette().background;
                if (now.r != themeBackground.r || now.g != themeBackground.g ||
                    now.b != themeBackground.b) {
                    pane.v.setDefaultBackground(themeBackground);
                }
            });
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
        bool eventThisRound = false;
        while (SDL_PollEvent(&event)) {
            eventThisRound = true;
            activeUntil = SDL_GetTicks64() + kActiveMs;
            lastActivity = SDL_GetTicks64();
            if (event.type == SDL_WINDOWEVENT) {
                // SDL_PollEvent pumps window-system changes after the update at
                // the start of this iteration. Refresh before processing any
                // mouse events queued behind a resize or maximize.
                updateWindow();
                if (mouseDebug) {
                    const auto flags = SDL_GetWindowFlags(window.get());
                    const auto &size = wv.window();
                    std::fprintf(stderr, "[window-mouse] window event=%u data=%d,%d flags=0x%x view=%dx%d\n",
                                 unsigned(event.window.event), event.window.data1, event.window.data2,
                                 unsigned(flags), size.width, size.height);
                }
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
                // Geometry and pointer coordinates must use the same current
                // drawable size even if a resize was pumped with this event.
                updateWindow();
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
                if (mouseDebug && !motion) {
                    const auto flags = SDL_GetWindowFlags(window.get());
                    const auto &size = wv.window();
                    std::fprintf(stderr, "[window-mouse] %s button=%u clicks=%u raw=%d,%d viewpos=%d,%d view=%dx%d flags=0x%x consumed=%d action=%s pane=%d\n",
                                 down ? "down" : "up", unsigned(event.button.button), unsigned(event.button.clicks),
                                 rawX, rawY, x, y, size.width, size.height, unsigned(flags), int(r.consumed),
                                 r.action.empty() ? "-" : r.action.c_str(), int(r.pane.has_value()));
                }
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
                if (key == SDLK_ESCAPE && !wv.hasLayer() &&
                    cancelReplacement(workspace.focusedPaneId())) {
                    notify("attachment cancelled; the original terminal is "
                           "unchanged");
                    continue;
                }
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
        const double layoutTickAt = perfLog.enabled() ? perfClockMs() : 0;
        afterLayout(wv.tick(nowMs));
        if (perfLog.enabled()) {
            perfLog.second.counters.layoutMs += perfClockMs() - layoutTickAt;
        }
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
        const double pumpRoundAt = perfLog.enabled() ? perfClockMs() : 0;
        workspace.forEachPane([&](PaneId id, NativePane &pane, bool visible) {
            const double pumpAt = perfLog.enabled() ? perfClockMs() : 0;
            pane.pump();
            if (perfLog.enabled() && pane.settingsStartedAt != 0 &&
                !pane.settingsParsed && pane.v.outputBytes() > 0) {
                pane.settingsParsed = true;
                perfLog.phase("settings_first_parsed_output",
                              perfClockMs() - pane.settingsStartedAt);
            }
            if (perfLog.enabled()) {
                auto &samples = id == workspace.focusedPaneId()
                                    ? perfLog.second.guiPumpFocused
                                : visible ? perfLog.second.guiPumpVisible
                                          : perfLog.second.guiPumpHidden;
                samples.add(perfClockMs() - pumpAt);
            }
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
        if (perfLog.enabled())
            perfLog.second.guiPumpRound.add(perfClockMs() - pumpRoundAt);
        for (auto it = pendingReplacements.begin();
             it != pendingReplacements.end();) {
            auto *original = workspace.pane(it->pane);
            AttachReplacementState state;
            state.originalSame =
                original == it->original && original && original->daemon &&
                original->daemon->sessionId() == it->originalSession;
            state.originalLive = original && !original->v.ended();
            if (state.originalSame && state.originalLive)
                it->staged->pump();
            const auto *link = it->staged->daemon.get();
            state.failed =
                !link ||
                link->session().state() == ClientSession::State::Failed ||
                link->session().state() == ClientSession::State::Exited;
            state.targetLive = !it->staged->v.ended();
            state.running = link && link->session().state() ==
                                        ClientSession::State::Running;
            state.targetMatches =
                !state.running || link->sessionId() == it->target;
            const auto decision = replacementDecision(
                state, std::int64_t(SDL_GetTicks64()), it->deadline);
            if (decision == ReplacementDecision::Wait) {
                ++it;
                continue;
            }
            if (decision == ReplacementDecision::Commit) {
                auto old = workspace.replacePaneAnywhere(it->pane, it->staged);
                if (old) {
                    workspace.pane(it->pane)->endInDaemon = true;
                    old->endInDaemon = it->endOld;
                    // The tree already owns the verified live new pane. Only
                    // now may destruction send Close for the old daemon
                    // terminal.
                    old.reset();
                    notify(it->endOld ? "terminal replaced"
                                      : "terminal replaced; the old one keeps "
                                        "running in the daemon");
                }
            }
            else {
                notify("attachment failed or cancelled; the original terminal "
                       "is unchanged");
            }
            it = pendingReplacements.erase(it);
        }
        for (auto it = deliveries.begin(); it != deliveries.end();) {
            NativePane *pane = workspace.pane(it->pane);
            const bool up = pane && (!pane->daemon || pane->daemon->session().state() == ClientSession::State::Running);
            if (up) {
                pane->v.note(it->note);
                if (!it->input.empty()) {
                    send(*pane, it->input);
                }
            }
            if (up || !pane || SDL_GetTicks64() > it->until) {
                it = deliveries.erase(it);
            }
            else {
                ++it;
            }
        }
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
        const double layoutAt = perfLog.enabled() ? perfClockMs() : 0;
        const bool blinkOn = now >= lastActivity + kBlinkTimeoutMs || (now / 500) % 2 == 0;
        if (ownFrame) {
            wv.setWindowMaximized((SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) != 0);
        }
        const FramePlan plan = wv.plan();
        if (ownFrame) { // What SDL's hit test reads: the handle is the strip's gaps.
            hitState.set(borderless::geometryOf(
                window.get(), plan.strip, plan.stripRect.h, backend.cellWidth(), drawableSize().first, insetNow().left));
        }
        const Palette stripPalette = wv.stripPalette();
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
        extra = extra * 31 + (alignCellsBottom ? 1u : 0u);
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
        // A flood of output (a replay after a reattach, `cat` of a big file)
        // changes the frame in every round; the model has taken it all, so
        // drawing at most ten times a second shows the same end result
        // (docs/CPU.md "Replay"). Input, a resize or an expose draws at once.
        std::uint64_t ingestNow = 0;
        workspace.forEachPane([&](PaneId, NativePane &pane, bool) {
            ingestNow += pane.view().outputBytes();
        });
        const bool flooded =
            ingestNow > ingestSeen && ingestNow - ingestSeen > 32768;
        ingestSeen = ingestNow;
        const bool hold =
            holdFloodDraw(settingsWatch.settings().flag("drawBatching"),
                          flooded,
                          eventThisRound || forceRedraw,
                          std::int64_t(now - lastDrawMs));
        if (perfLog.enabled()) {
            perfLog.second.counters.layoutMs += perfClockMs() - layoutAt;
        }
        if (!hold && (forceRedraw || signatures.full != lastDrawnSignature)) {
            const double drawAt = perfLog.enabled() ? perfClockMs() : 0;
            lastDrawMs = now;
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
            const auto flags = SDL_GetWindowFlags(window.get());
            const int stripHeight = plan.stripRect.h;
            const auto decoration =
                splitDecoration(plan.dividers,
                                {0,
                                 stripHeight,
                                 wv.window().width,
                                 std::max(0, wv.window().height - stripHeight)},
                                ownFrame,
                                (flags & SDL_WINDOW_MAXIMIZED) != 0,
                                (flags & SDL_WINDOW_FULLSCREEN) != 0);
            const auto paintDecoration = [&](PixelRect r, Rgb8 color) {
                backend.drawRect({r.x + ox, r.y, r.w, r.h}, color);
            };
            paintSplitGutters(decoration, stripPalette, paintDecoration);
            for (const auto &d : drawn) {
                const auto &pp = *d.plan;
                backend.render(
                    d.frame,
                    PaneArea{{pp.rect.x + ox, pp.rect.y, pp.rect.w, pp.rect.h},
                             {pp.scrollbar.x + ox, pp.scrollbar.y, pp.scrollbar.w, pp.scrollbar.h},
                             alignCellsBottom});
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
            for (const auto &edge : decoration.outer)
                paintDecoration(edge, splitEdgeColor(stripPalette));
            for (std::size_t i = 0; i < layerFrames.size(); ++i) {
                const auto &layer = plan.layers[i];
                backend.render(layerFrames[i], PaneArea{{layer.rect.x + ox, layer.rect.y, layer.rect.w, layer.rect.h}, {}});
            }
            if (ownFrame) { // The window has no frame: the border (outside the view) shows where its edge is.
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(SDL_GetRenderer(window.get()), &w, &h);
                const bool maximized = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
                const bool fullscreen = (SDL_GetWindowFlags(window.get()) &
                                         SDL_WINDOW_FULLSCREEN) != 0;
                for (const auto &edge : frontend::borderRects(
                         w, h, maximized, frontend::kBorderWidth, fullscreen)) {
                    backend.drawRect({edge.x, edge.y, edge.w, edge.h}, frontend::borderColor(stripPalette));
                }
                std::vector<PixelRect> scrollbars;
                for (const auto &d : drawn) {
                    if (!d.frame.scrollbar.visible)
                        continue;
                    const auto &pp = *d.plan;
                    if (pp.rect.x + pp.rect.w == wv.window().width) {
                        const int width =
                            std::min(pp.rect.w, SdlBackend::scrollbarStrip());
                        scrollbars.push_back({pp.rect.x + pp.rect.w - width,
                                              pp.rect.y,
                                              width,
                                              pp.rect.h});
                    }
                }
                for (const auto &edge : ownFrameDarkEdges(
                         {0,
                          stripHeight,
                          wv.window().width,
                          std::max(0, wv.window().height - stripHeight)},
                         plan.dividers,
                         scrollbars,
                         ownFrame,
                         maximized,
                         fullscreen))
                    paintDecoration(edge, splitEdgeColor(stripPalette));
                for (const auto &edge : frontend::outerFrameEdges(w, h, maximized, fullscreen))
                    backend.drawRect({edge.x, edge.y, edge.w, edge.h}, splitEdgeColor(stripPalette));
            }
            backend.present();
            if (daemonOptions.serverReadyFd >= 0) {
                frontend::sendServerStatus(daemonOptions.serverReadyFd, 'R');
                ::close(daemonOptions.serverReadyFd);
                daemonOptions.serverReadyFd = -1;
            }
            if (perfLog.enabled()) {
                for (const auto &d : drawn) {
                    if (auto *pane = workspace.pane(d.plan->id);
                        pane && pane->settingsStartedAt != 0 &&
                        pane->settingsParsed && !pane->settingsPresented) {
                        pane->settingsPresented = true;
                        perfLog.phase("settings_first_present",
                                      perfClockMs() - pane->settingsStartedAt);
                    }
                }
                perfLog.second.counters.drawMs += perfClockMs() - drawAt;
            }
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
    if (!keepTerminals || gShutdownSignal == 0) recovery.retire();
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
