// Browser front end: terminals (model, VT parser) in panes of a workspace,
// drawn by the SDL renderer on a canvas, each connected to matterm-server
// through its own ClientSession (WebSocket). With no server URL it runs the
// pretend system (src/fakeos) through ScriptedLink, as a single pane.
// The layout (tabs, panes, strip, dividers) comes from WorkspaceView; this file
// owns the panes, the SDL events and the drawing.
#include "cellbuffer.h"
#include "clientsession.h"
#include "cursorglide.h"
#include "framesignature.h"
#include "inputencoder.h"
#include "layoutsnapshot.h"
#include "palette.h"
#include "scriptedlink.h"
#include "scriptedprogram.h"
#include "sdlbackend.h"
#include "sdlinput.h"
#include "terminalview.h"
#include "touchgesture.h"
#include "ui/commandpalette.h"
#include "ui/dialogs.h"
#include "websocketchannel.h"
#include "workspaceview.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace frontend;
using MouseEvent = InputEncoder::MouseEvent;

namespace {
// Trace to the browser console (stderr), only with ?debug in the page URL.
bool gDebug = false; // ?debug
#define DBG(...)                                                               \
    do {                                                                       \
        if (gDebug) {                                                          \
            std::fprintf(stderr, "[dbg] " __VA_ARGS__);                        \
            std::fputc('\n', stderr);                                          \
        }                                                                      \
    } while (0)
// One terminal of the window: the link that feeds it and the TerminalView that
// owns everything the link produced. Pointer state (a drag, mouse capture)
// belongs to App, not to a pane.
struct WebPane {
    explicit WebPane(const TerminalView::Config &config)
        : terminal(config) {}
    ~WebPane(); // A live session is told to end (see retireSession).
    WebPane(const WebPane &) = delete;
    WebPane &operator=(const WebPane &) = delete;
    std::unique_ptr<WebSocketChannel> channel;
    std::unique_ptr<ClientSession> session;
    std::unique_ptr<ScriptedLink> demo; // The pretend system, no server.
    TerminalLink *link = nullptr;       // session or demo.
    std::string requested;              // The session id asked for (or empty).
    std::string attachId;               // What Hello carried: an id, "local:<id>" or empty.
    bool changed = false;               // Output arrived since the last frame.
    bool hinted = false;                // What Enter does now was written.
    // Last: it listens to the link above, so it is destroyed first.
    TerminalView terminal;

    TerminalView &view() {
        return terminal;
    }
    const TerminalView &view() const {
        return terminal;
    }
};

struct App {
    SDL_Window *window = nullptr;
    std::unique_ptr<SdlBackend> backend;
    std::unique_ptr<Workspace<WebPane>> workspace;
    std::unique_ptr<WorkspaceView<WebPane>> layout;
    std::string url; // WebSocket URL; empty: the pretend system.
    CursorGlide::Config glideConfig;
    const Palette chromePalette; // Strip, gaps and dividers.
    Uint64 last = 0;
    bool skipText = false;
    // Mouse: application reporting, or local selection. The pane that holds a
    // press (capture or selection drag) keeps the pointer until the release.
    bool mouseCaptured = false;
    PaneId capturePane = 0;
    MouseEvent::Button heldButton = MouseEvent::Button::None;
    std::optional<std::pair<int, int>> lastMouseCell;
    bool dragging = false;
    PaneId dragPane = 0;
    std::string title = "matterm"; // Base title, without the Ctrl-hover target.
    // Idle frames: skip render+present while nothing visible changed.
    std::optional<std::uint64_t> lastSignature;
    std::uint64_t lastStable = 0;
    // Idle throttling: full speed for a while after input or any visible
    // change (blink excluded), otherwise one full pass every idleMs.
    Uint64 activeUntil = 0;
    bool paused = false; // The page is out of view; see frame().
    Uint64 pauseChecked = 0;
    Uint64 lastFull = 0;
    // Touch: gesture state, scroll remainder in rows, and the font size in CSS
    // pixels (changed by pinch or Ctrl+wheel, applied between frames).
    TouchGesture touch;
    double touchRows = 0;
    int fontCss = 18;
    int pendingFontCss = 18;
    std::string shownTitle;
    int pointerShape = 0;       // 0 arrow, 1 columns, 2 rows.
    std::string pendingSession; // For the next pane the factory makes.
    // The laptop's shared terminals, offered by the server (POST /local-sessions): 0 not known
    // (or not offered), 1 offered. `localPick`: show the list when the answer arrives.
    int localOffered = 0;
    bool localAsking = false;
    bool localPick = false;
    std::vector<std::pair<std::string, bool>> localSessions; // id, attached
    std::string savedLayout;    // What sessionStorage holds.
    bool touchDivider = false;  // A finger is dragging a divider.
    int touchFinger = 0;
    bool replayTouch = false; // A tap is being replayed as a click.
    bool prefixShown = false; // The extra-keys button shows "armed".
    bool keyShift = false; // Shift held at the last key press (not live state).
    // Sessions of closed panes, waiting for the server to confirm the Close.
    struct Retired {
        std::unique_ptr<WebSocketChannel> channel;
        std::unique_ptr<ClientSession> session;
        Uint64 deadline = 0;
    };
    std::vector<Retired> closing;
    Uint64 reloadDeadline =
        0; // Reload to the login form once closing is empty.
    bool warnedVolatile = false; // The server keeps no terminals; said once.
    bool leaving = false;        // Reloading to the login form.
    std::unique_ptr<CellBuffer> stripCells; // The tab strip row as cells.

    WebPane *focused() const {
        return workspace ? workspace->focusedPane() : nullptr;
    }
    // Whether the layout keys (the prefix) are on; off in the static demo.
    bool layoutKeys() const {
        return layout && layout->config().prefixKeys;
    }
    // A session that is over needs the user (the pretend system never ends).
    bool ended(const WebPane &pane) const {
        return !url.empty() && pane.view().ended();
    }
};

App *gApp = nullptr;                      // For matterm_paste.
fakeos::ScriptedProgram *gChat = nullptr; // For matterm_chat_event.

// A closed pane must end its terminal on the server: dropping the connection
// would leave a persistent session behind (and count against the limits). The
// session sends Close and lives on, unseen, until the server confirms.
WebPane::~WebPane() {
    terminal.detach();
    if (session && gApp &&
        (session->state() == ClientSession::State::AwaitWelcome ||
         session->state() == ClientSession::State::Running)) {
        session->close();
        gApp->closing.push_back(
            {std::move(channel), std::move(session), SDL_GetTicks64() + 3000});
    }
}

// The `ws` query parameter (?ws=ws://127.0.0.1:8080/ws) or window.mattermWs
// (set by login.js). Empty: demo.
// A query parameter as text (empty when absent); for ?cmd= in the demo.
std::string queryParam(const char *name) {
    char *value = static_cast<char *>(EM_ASM_PTR(
        {
            const v =
                new URLSearchParams(location.search).get(UTF8ToString($0)) ||
                "";
            const n = lengthBytesUTF8(v) + 1;
            const p = _malloc(n);
            stringToUTF8(v, p, n);
            return p;
        },
        name));
    std::string text = value;
    std::free(value);
    return text;
}

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
double pixelRatio() {
    return EM_ASM_DOUBLE({ return window.devicePixelRatio || 1; });
}
int fontPixels(int cssPixels) {
    return fontDevicePixels(cssPixels, pixelRatio());
}

// The font size in CSS pixels is remembered per browser (localStorage can be
// unavailable or throw, then the default is used).
int loadFontSize() {
    const int value = EM_ASM_INT({
        try {
            return parseInt(localStorage.getItem("matterm.fontSize"), 10) || 0;
        }
        catch (e) {
            return 0;
        }
    });
    return value > 0 ? steppedFontSize(value, 0) : 18;
}
void saveFontSize(int cssPixels) {
    EM_ASM(
        {
            try {
                localStorage.setItem("matterm.fontSize", String($0));
            }
            catch (e) {
            }
        },
        cssPixels);
}

void setTitle(const std::string &title) {
    if (gApp) {
        gApp->title = title;
    }
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

std::int64_t nowMs() {
    return std::int64_t(SDL_GetTicks64());
}

protocol::Time clockNow() {
    return protocol::Time(std::chrono::milliseconds(SDL_GetTicks64()));
}

void sendTo(WebPane &pane, std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (pane.view().sendInput(bytes)) {
        pane.view().model().followTail();
    }
}

// To the focused pane's program.
void send(App &app, const std::string &bytes) {
    if (auto *pane = app.focused()) {
        sendTo(*pane, bytes);
    }
}

bool mouseTracking(const WebPane &pane) {
    return pane.view().modes().mouseTracking !=
           TerminalModes::MouseTracking::Off;
}

// Window (CSS-like SDL) coordinates to drawable pixels, which the layout uses.
std::pair<int, int> drawablePoint(const App &app, int x, int y) {
    int windowWidth = 0, windowHeight = 0;
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetWindowSize(app.window, &windowWidth, &windowHeight);
    SDL_GetRendererOutputSize(
        SDL_GetRenderer(app.window), &drawableWidth, &drawableHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return {x, y};
    }
    return {int(double(x) * drawableWidth / windowWidth),
            int(double(y) * drawableHeight / windowHeight)};
}

const PanePlan *findPlan(const FramePlan &plan, PaneId id) {
    for (const auto &pane : plan.panes) {
        if (pane.id == id) {
            return &pane;
        }
    }
    return nullptr;
}

// The text cell of a pane under a drawable-pixel point.
std::optional<TerminalModel::TextCell> cellInPane(const App &app,
                                                  const WebPane &pane,
                                                  const PanePlan &plan,
                                                  int x,
                                                  int y,
                                                  bool clamp) {
    return cellAt(pane.view().model(),
                  x - plan.rect.x,
                  y - plan.rect.y,
                  app.backend->cellWidth(),
                  app.backend->cellHeight(),
                  double(plan.columns) * app.backend->cellWidth(),
                  double(plan.rows) * app.backend->cellHeight(),
                  clamp);
}

// The link under the pointer in a pane, when the gesture asks for local links
// (Ctrl+hover). Polled after model updates so a stationary pointer cannot keep
// stale text after output or scrolling.
std::optional<UrlMatch> hoveredLink(const App &app,
                                    const WebPane &pane,
                                    const PanePlan &plan) {
    const auto mod = modifiers(SDL_GetModState());
    constexpr auto focus = SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS;
    if (!localLinkGesture(
            mod.control, mod.shift, mouseTracking(pane), app.mouseCaptured) ||
        (SDL_GetWindowFlags(app.window) & focus) != focus) {
        return std::nullopt;
    }
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const auto [x, y] = drawablePoint(app, mx, my);
    const auto cell = cellInPane(app, pane, plan, x, y, false);
    return cell ? urlAt(pane.view().model(), *cell) : std::nullopt;
}

void forgetLayout();

// Back to the login form, once the server has confirmed the Close of the
// terminals that were just closed (a reload would drop them unannounced).
void requestReload(App &app) {
    app.reloadDeadline = SDL_GetTicks64() + 3000;
}

// The layout changed (a pane, tab or split came or went): redraw, and let go of
// pointer state that belonged to what was shown.
void layoutChanged(App &app, bool empty) {
    app.lastSignature.reset();
    app.dragging = false;
    app.mouseCaptured = false;
    app.heldButton = MouseEvent::Button::None;
    app.lastMouseCell.reset();
    app.touchRows = 0;
    app.skipText = false;
    app.activeUntil = SDL_GetTicks64() + 500;
    if (empty) {
        // The last pane is gone: back to the login form.
        forgetLayout();
        requestReload(app);
    }
}

void sendMouse(App &app,
               WebPane &pane,
               MouseEvent::Type type,
               MouseEvent::Button button,
               const std::optional<TerminalModel::TextCell> &cell,
               bool dedupe) {
    if (!cell) {
        return;
    }
    auto &model = pane.view().model();
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
    sendTo(pane, InputEncoder::mouse(report, pane.view().modes()));
}

// ---- layout keys (the prefix) ----

std::string prefixBytes(const App &app) {
    const auto &prefix = app.layout->config().prefix.prefix;
    return InputEncoder::control(char(prefix.key), prefix.alt);
}

// The command palette (Ctrl+B ':', the strip's button): an overlay over the
// panes.
void openPalette(App &app) {
    std::vector<CommandEntry> extra;
    if (app.localOffered == 1) {
        extra.push_back({"local.open",
                         "Local terminals...",
                         "Take over a terminal that is shared from the laptop",
                         std::nullopt,
                         {},
                         {}});
    }
    app.layout->pushLayer(std::make_unique<ui::CommandPalette>(
        buildCommandRegistry(app.layout->config().prefix, extra)));
    layoutChanged(app, false);
}

// ---- the laptop's shared terminals (docs/TABS_PLAN.md section 11) ----

// The ids this window took over from the laptop, remembered for the tab (sessionStorage), so a
// reload attaches to them as local terminals again. Browser storage can be missing: then a
// reload shows them as unavailable, nothing worse.
void rememberLocal(const std::string &id) {
    EM_ASM(
        {
            try {
                const have = (sessionStorage.getItem("matterm.local") || "").split(",").filter(Boolean);
                const id = UTF8ToString($0);
                if (!have.includes(id)) {
                    have.push(id);
                }
                sessionStorage.setItem("matterm.local", have.slice(-32).join(","));
            } catch (e) {
            }
        },
        id.c_str());
}

bool isLocal(const std::string &id) {
    return EM_ASM_INT(
               {
                   try {
                       return (sessionStorage.getItem("matterm.local") || "").split(",").includes(UTF8ToString($0)) ? 1 : 0;
                   } catch (e) {
                       return 0;
                   }
               },
               id.c_str()) != 0;
}

// Asks the server which shared terminals of the local daemon it offers (POST, so that the
// browser sends an Origin header; the login cookie goes along). The answer is picked up by
// pollLocal(). A server that does not expose them answers 404: the menu entry stays hidden.
void askLocalSessions(App &app) {
    if (app.url.empty() || app.localAsking) {
        return;
    }
    app.localAsking = true;
    EM_ASM(
        {
            // No object literal: commas outside parentheses split the macro arguments.
            const state = Object.create(null);
            state.done = false;
            state.status = 0;
            state.lines = "";
            globalThis.mattermLocal = state;
            let url;
            try {
                const u = new URL(UTF8ToString($0), location.href);
                u.protocol = u.protocol === "wss:" ? "https:" : "http:";
                u.pathname = "/local-sessions";
                u.search = "";
                url = u.toString();
            } catch (e) {
                state.done = true;
                return;
            }
            fetch(url, {method : "POST", credentials : "include"})
                .then((r) => {
                    state.status = r.status;
                    return r.ok ? r.json() : null;
                })
                .then((j) => {
                    if (j && Array.isArray(j.sessions)) {
                        state.lines =
                            j.sessions
                                .filter((s) => typeof s.id === "string" &&
                                               /^[0-9a-f]{32}$/.test(s.id))
                                .map((s) => s.id + " " + (s.attached ? "1" : "0"))
                                .join("\n");
                    }
                    state.done = true;
                })
                .catch(() => { state.done = true; });
        },
        app.url.c_str());
}

void showLocalPicker(App &app) {
    std::vector<ui::PickList::Item> items;
    for (const auto &[id, attached] : app.localSessions) {
        items.push_back({id.substr(0, 8),
                         attached ? "in use" : "",
                         "local.attach:" + id});
    }
    app.layout->pushLayer(std::make_unique<ui::PickList>(
        "Local terminals", std::move(items), "Nothing is shared from the laptop"));
    layoutChanged(app, false);
}

// Called every frame: takes the server's answer when it has come.
void pollLocal(App &app) {
    if (!app.localAsking ||
        EM_ASM_INT({ return globalThis.mattermLocal && globalThis.mattermLocal.done ? 1 : 0; }) == 0) {
        return;
    }
    app.localAsking = false;
    const int status = EM_ASM_INT({ return globalThis.mattermLocal.status | 0; });
    char *raw = static_cast<char *>(EM_ASM_PTR({
        const v = globalThis.mattermLocal.lines || "";
        const n = lengthBytesUTF8(v) + 1;
        const p = _malloc(n);
        stringToUTF8(v, p, n);
        return p;
    }));
    const std::string lines = raw;
    std::free(raw);
    if (status == 200) {
        app.localOffered = 1;
        app.localSessions.clear();
        std::size_t at = 0;
        while (at < lines.size()) {
            const auto end = std::min(lines.find('\n', at), lines.size());
            const std::string line = lines.substr(at, end - at);
            at = end + 1;
            if (line.size() == 34 && LayoutSnapshot::validId(line.substr(0, 32))) {
                app.localSessions.emplace_back(line.substr(0, 32), line.back() == '1');
            }
        }
    }
    else if (status == 404 || status == 401) {
        app.localOffered = 0; // Not exposed here (or not logged in).
    }
    if (app.localPick) {
        app.localPick = false;
        if (app.localOffered == 1) {
            showLocalPicker(app);
        }
    }
}

// A new tab or split could not be made: say so in the focused terminal.
void refusedNotice(App &app) {
    if (auto *pane = app.focused()) {
        pane->view().note(
            "no new terminal: at most 16 in this window, and the space must "
            "fit them");
    }
}

// What a layer or the strip asked of the application itself (a CommandEntry id, possibly
// "id:argument").
void handleAction(App &app, const std::string &action) {
    const auto colon = action.find(':');
    const std::string id = action.substr(0, colon);
    const std::string argument = colon == std::string::npos ? std::string() : action.substr(colon + 1);
    if (id == "local.open") {
        app.localPick = true;
        askLocalSessions(app);
    }
    else if (id == "local.attach" && LayoutSnapshot::validId(argument)) {
        // A new tab whose terminal is the laptop's: the server bridges "local:<id>" to the daemon.
        rememberLocal(argument);
        app.pendingSession = "local:" + argument;
        const Outcome made = app.workspace->apply(Command::NewTab);
        app.pendingSession.clear();
        if (made == Outcome::Ignored) {
            refusedNotice(app);
        }
        layoutChanged(app, false);
    }
}

// Feeds a key to the layout. True when the layout took it (a command, the
// prefix itself, or a literal prefix that was sent); false: the key goes on to
// the program (after the prefix, if the layout says so).
bool layoutKey(App &app, const KeyChord &chord) {
    const auto result = app.layout->key(chord, nowMs());
    DBG("layout key=%d ctrl=%d shift=%d alt=%d -> commands=%zu changed=%d "
        "empty=%d fwdPrefix=%d fwdKey=%d tabs=%zu panes=%zu",
        chord.key,
        chord.ctrl,
        chord.shift,
        chord.alt,
        result.commands.size(),
        result.changed,
        result.empty,
        result.forwardPrefix,
        result.forwardKey,
        app.workspace->tabCount(),
        app.workspace->paneCount());
    if (result.changed || result.empty) {
        layoutChanged(app, result.empty);
    }
    if (!result.refused.empty()) {
        refusedNotice(app);
    }
    if (result.openPalette) {
        openPalette(app);
    }
    if (!result.action.empty()) {
        handleAction(app, result.action);
    }
    if (result.forwardPrefix) {
        send(app, prefixBytes(app));
    }
    return !result.forwardKey;
}

bool isModifierKey(SDL_Keycode sym) {
    switch (sym) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
    case SDLK_CAPSLOCK:
    case SDLK_NUMLOCKCLEAR:
    case SDLK_SCROLLLOCK:
    case SDLK_MODE:
        return true;
    default:
        return false;
    }
}

// The chord of a key press that has no text of its own (named keys, Ctrl+key).
// Printable keys arrive as text and are fed from there, so that "%" and "\""
// are the characters the layout produces, not the keys.
std::optional<KeyChord> chordFor(SDL_Keycode sym,
                                 const InputEncoder::Modifiers &mod) {
    int key = 0;
    switch (sym) {
    case SDLK_UP:
        key = keys::Up;
        break;
    case SDLK_DOWN:
        key = keys::Down;
        break;
    case SDLK_LEFT:
        key = keys::Left;
        break;
    case SDLK_RIGHT:
        key = keys::Right;
        break;
    case SDLK_PAGEUP:
        key = keys::PageUp;
        break;
    case SDLK_PAGEDOWN:
        key = keys::PageDown;
        break;
    case SDLK_ESCAPE:
        key = keys::Escape;
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        key = keys::Enter;
        break;
    case SDLK_TAB:
        key = keys::Tab;
        break;
    case SDLK_BACKSPACE:
        key = keys::Backspace;
        break;
    default:
        if (mod.control && sym > 0 && sym < 128) {
            key = int(sym);
        }
        break;
    }
    if (key == 0) {
        return std::nullopt;
    }
    return KeyChord{key, mod.control, mod.shift, mod.alt};
}

void handleKey(App &app, const SDL_KeyboardEvent &key) {
    auto *pane = app.focused();
    if (!pane) {
        return;
    }
    const auto mod = modifiers(SDL_Keymod(key.keysym.mod));
    const auto sym = key.keysym.sym;
    if (!isModifierKey(sym)) {
        app.keyShift = mod.shift; // From the event itself: text arrives later.
    }
    DBG("keydown sym=%d ctrl=%d shift=%d alt=%d repeat=%d layoutKeys=%d",
        int(sym),
        mod.control,
        mod.shift,
        mod.alt,
        key.repeat,
        app.layoutKeys());
    if (app.layoutKeys() && !key.repeat && !isModifierKey(sym)) {
        if (const auto chord = chordFor(sym, mod)) {
            if (layoutKey(app, *chord)) {
                app.skipText = false;
                return;
            }
            pane = app.focused();
            if (!pane) {
                return;
            }
        }
    }
    auto &view = pane->view();
    const auto &modes = view.modes();
    if (mod.control && mod.shift && sym == SDLK_c) {
        // Ctrl+Shift+V (paste) is handled by the page's paste event.
        if (!key.repeat && view.selection &&
            view.selection->valid(view.model())) {
            if (const auto text = view.selection->text(view.model())) {
                copyToClipboard(*text);
            }
            else {
                setTitle("matterm | copy failed: selection exceeds 1 MiB");
            }
        }
    }
    else if (mod.control && mod.shift && sym == SDLK_a) {
        if (!key.repeat) {
            view.selection = Selection::all(view.model());
            app.dragging = false;
        }
    }
    else if (mod.control && mod.shift && sym == SDLK_v) {
        // Nothing to send: see above.
    }
    else if (mod.shift && (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN)) {
        view.model().scrollBy((sym == SDLK_PAGEUP ? 1.0 : -1.0) *
                              view.model().rows());
    }
    else if (mod.control && mod.shift && sym == SDLK_HOME) {
        view.model().scrollToOldest();
    }
    else if (mod.control && mod.shift && sym == SDLK_END) {
        view.model().followTail();
    }
    else if (modes.applicationKeypad && keypadKey(sym)) {
        app.skipText = true;
        sendTo(*pane, InputEncoder::keypad(*keypadKey(sym), mod, modes));
    }
    else if (const auto encoded = special(sym)) {
        sendTo(*pane, InputEncoder::key(*encoded, mod, modes));
    }
    else if (mod.control && sym >= 0 && sym <= 127) {
        sendTo(*pane, InputEncoder::control(char(sym), mod.alt));
    }
}

// Text from the keyboard (or the on-screen one): the layout sees a single
// character first (the second key of the prefix), the program gets the rest.
void handleText(App &app, const char *text, bool alt) {
    DBG("text '%s' layoutKeys=%d", text ? text : "", app.layoutKeys());
    if (app.layoutKeys() && text && *text) {
        const auto *p = reinterpret_cast<const unsigned char *>(text);
        const std::size_t length = p[0] < 0x80   ? 1
                                   : p[0] < 0xe0 ? 2
                                   : p[0] < 0xf0 ? 3
                                                 : 4;
        if (std::strlen(text) == length) {
            char32_t cp = length == 1   ? p[0]
                          : length == 2 ? p[0] & 0x1f
                          : length == 3 ? p[0] & 0x0f
                                        : p[0] & 0x07;
            for (std::size_t i = 1; i < length; ++i) {
                cp = (cp << 6) | (p[i] & 0x3f);
            }
            // Shift as it was when the key went down; by the time the text
            // arrives the live state may be gone.
            const bool shift = std::exchange(app.keyShift, false);
            // Shift is part of the character. Some key sources (automation, a
            // US keyboard model) report Shift+5 / Shift+' as the unshifted
            // text; the prefix commands are '%' and '"'.
            if (shift && cp == U'5') {
                cp = U'%';
            }
            else if (shift && cp == U'\'') {
                cp = U'"';
            }
            else if (shift && cp == U';') {
                cp = U':';
            }
            if (layoutKey(app, KeyChord{int(cp), false, false, alt})) {
                return;
            }
        }
    }
    send(app, InputEncoder::text(text, alt));
}

void handleWheel(App &app, const SDL_MouseWheelEvent &wheel) {
    double amount = wheel.preciseY != 0 ? wheel.preciseY : wheel.y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        amount = -amount;
    }
    if (modifiers(SDL_GetModState()).control) {
        // Ctrl+wheel: font size, one step per notch (applied before the next
        // frame; the application never sees it).
        app.pendingFontCss =
            steppedFontSize(app.pendingFontCss, amount > 0 ? 1 : -1);
        return;
    }
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const auto [x, y] = drawablePoint(app, mx, my);
    const auto result = app.layout->wheel(x, y, amount);
    if (result.changed) {
        layoutChanged(app, false);
    }
    if (result.consumed || !result.pane) {
        return;
    }
    auto *pane = app.workspace->pane(*result.pane);
    const auto plan = app.layout->plan();
    const auto *placed = findPlan(plan, *result.pane);
    if (!pane || !placed) {
        return;
    }
    auto &view = pane->view();
    const auto &modes = view.modes();
    if (mouseTracking(*pane) && !modifiers(SDL_GetModState()).shift) {
        const auto cell = cellInPane(app, *pane, *placed, x, y, true);
        const auto button = amount > 0 ? MouseEvent::Button::WheelUp
                                       : MouseEvent::Button::WheelDown;
        const int steps = std::clamp(int(std::ceil(std::abs(amount))), 1, 10);
        for (int i = 0; i < steps; ++i) {
            sendMouse(app, *pane, MouseEvent::Type::Press, button, cell, false);
        }
    }
    else if (view.model().alternateScreen()) {
        const auto key =
            amount > 0 ? InputEncoder::Key::Up : InputEncoder::Key::Down;
        const int count = int(std::ceil(std::min(10.0, std::abs(amount)) * 3));
        for (int i = 0; i < count; ++i) {
            sendTo(*pane, InputEncoder::key(key, {}, modes));
        }
    }
    else {
        view.model().scrollBy(amount * 3);
    }
}

// The pane under a point in window coordinates (a finger), with its plan.
struct Under {
    WebPane *pane = nullptr;
    PanePlan plan;
    int x = 0, y = 0; // Drawable pixels.
};
Under paneAtWindow(App &app, int wx, int wy) {
    Under under;
    const auto [x, y] = drawablePoint(app, wx, wy);
    under.x = x;
    under.y = y;
    const PaneId id = app.workspace->layoutActive().paneAt(x, y);
    const auto plan = app.layout->plan();
    if (const auto *placed = findPlan(plan, id)) {
        under.plan = *placed;
        under.pane = app.workspace->pane(id);
    }
    return under;
}

// One-finger drag. `rows` is the distance in text rows (positive: the finger
// moved down, so older text comes into view, like wheel up). Application mouse
// reporting gets wheel presses at the finger, the alternate screen gets arrow
// keys, as the wheel does; the primary screen scrolls smoothly.
void touchScroll(App &app, double rows, int wx, int wy) {
    if (app.layout->hasLayer()) {
        return; // An overlay is open: the panes below do not scroll.
    }
    const auto under = paneAtWindow(app, wx, wy);
    if (!under.pane) {
        return;
    }
    auto &pane = *under.pane;
    auto &view = pane.view();
    const auto &modes = view.modes();
    if (!mouseTracking(pane) && !view.model().alternateScreen()) {
        view.model().scrollBy(rows);
        return;
    }
    app.touchRows += rows;
    const int steps = int(std::trunc(app.touchRows));
    app.touchRows -= steps;
    const auto cell = cellInPane(app, pane, under.plan, under.x, under.y, true);
    for (int i = 0; i < std::min(std::abs(steps), 20); ++i) {
        if (mouseTracking(pane)) {
            sendMouse(app,
                      pane,
                      MouseEvent::Type::Press,
                      steps > 0 ? MouseEvent::Button::WheelUp
                                : MouseEvent::Button::WheelDown,
                      cell,
                      false);
        }
        else {
            sendTo(pane,
                   InputEncoder::key(steps > 0 ? InputEncoder::Key::Up
                                               : InputEncoder::Key::Down,
                                     {},
                                     modes));
        }
    }
}

void handleEvent(App &app, const SDL_Event &event);

// A tap is a click: replay it as left button press and release, which takes
// the same path as a mouse (application reporting, or local selection).
void touchTap(App &app, int x, int y) {
    SDL_Event press{};
    press.type = SDL_MOUSEBUTTONDOWN;
    press.button.button = SDL_BUTTON_LEFT;
    press.button.state = SDL_PRESSED;
    press.button.clicks = 1;
    press.button.x = x;
    press.button.y = y;
    app.replayTouch =
        true; // A finger: the strip and dividers get a bigger reach.
    handleEvent(app, press);
    SDL_Event release = press;
    release.type = SDL_MOUSEBUTTONUP;
    release.button.state = SDL_RELEASED;
    handleEvent(app, release);
    app.replayTouch = false;
}

void applyTouch(App &app, const std::vector<TouchGesture::Action> &actions) {
    using Kind = TouchGesture::Kind;
    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSize(app.window, &windowWidth, &windowHeight);
    int drawableHeight = 0;
    SDL_GetRendererOutputSize(
        SDL_GetRenderer(app.window), nullptr, &drawableHeight);
    // Window units to text rows.
    const double rowsPerUnit =
        windowHeight > 0 && app.backend->cellHeight() > 0
            ? double(drawableHeight) / windowHeight / app.backend->cellHeight()
            : 0;
    for (const auto &action : actions) {
        const int x = int(action.x), y = int(action.y);
        switch (action.kind) {
        case Kind::Scroll:
            touchScroll(app, action.amount * rowsPerUnit, x, y);
            break;
        case Kind::Tap:
            touchTap(app, x, y);
            break;
        case Kind::LongPress: {
            // Select the word under the finger; dragging extends it. An
            // application that has the mouse keeps it (no local selection).
            const auto under = paneAtWindow(app, x, y);
            if (under.pane && !mouseTracking(*under.pane)) {
                if (const auto cell = cellInPane(app,
                                                 *under.pane,
                                                 under.plan,
                                                 under.x,
                                                 under.y,
                                                 false)) {
                    auto &view = under.pane->view();
                    selectAt(view.selection, view.model(), *cell, false, 2);
                    app.dragging = true;
                    app.dragPane = under.plan.id;
                    EM_ASM({
                        try {
                            navigator.vibrate(15);
                        }
                        catch (e) {
                        }
                    });
                }
            }
            break;
        }
        case Kind::SelectExtend: {
            auto *pane = app.workspace->pane(app.dragPane);
            const auto plan = app.layout->plan();
            const auto *placed = findPlan(plan, app.dragPane);
            if (app.dragging && pane && placed && pane->view().selection &&
                pane->view().selection->valid(pane->view().model())) {
                const auto [dx, dy] = drawablePoint(app, x, y);
                if (const auto cell =
                        cellInPane(app, *pane, *placed, dx, dy, true)) {
                    pane->view().selection->extend(pane->view().model(), *cell);
                }
            }
            break;
        }
        case Kind::SelectEnd: {
            auto *pane = app.workspace->pane(app.dragPane);
            if (app.dragging && pane && pane->view().selection &&
                pane->view().selection->valid(pane->view().model())) {
                // No Ctrl+Shift+C on a phone: finishing a selection copies it.
                if (const auto text =
                        pane->view().selection->text(pane->view().model())) {
                    copyToClipboard(*text);
                }
            }
            app.dragging = false;
            break;
        }
        case Kind::Pinch:
            app.pendingFontCss =
                steppedFontSize(app.pendingFontCss, int(action.amount));
            break;
        }
    }
}

// What the pointer shows over the layout's chrome.
void setPointer(App &app, MouseResult::Cursor cursor) {
    const int shape = cursor == MouseResult::Cursor::ResizeColumns ? 1
                      : cursor == MouseResult::Cursor::ResizeRows  ? 2
                                                                   : 0;
    if (shape == app.pointerShape) {
        return;
    }
    app.pointerShape = shape;
    static SDL_Cursor *cursors[3] = {nullptr, nullptr, nullptr};
    if (!cursors[shape]) {
        cursors[shape] =
            SDL_CreateSystemCursor(shape == 1   ? SDL_SYSTEM_CURSOR_SIZEWE
                                   : shape == 2 ? SDL_SYSTEM_CURSOR_SIZENS
                                                : SDL_SYSTEM_CURSOR_ARROW);
    }
    if (cursors[shape]) {
        SDL_SetCursor(cursors[shape]);
    }
}

// Mouse events go to the layout first (strip, dividers, scrollbars, focus);
// what is left is for the pane under the pointer, or the pane that holds the
// press.
void handleMouse(App &app, const SDL_Event &event) {
    MouseInput in;
    int x = 0, y = 0;
    if (event.type == SDL_MOUSEMOTION) {
        in.type = MouseInput::Type::Move;
        x = event.motion.x;
        y = event.motion.y;
    }
    else {
        in.type = event.type == SDL_MOUSEBUTTONDOWN ? MouseInput::Type::Down
                                                    : MouseInput::Type::Up;
        in.button = event.button.button == SDL_BUTTON_LEFT
                        ? MouseInput::Button::Left
                    : event.button.button == SDL_BUTTON_MIDDLE
                        ? MouseInput::Button::Middle
                    : event.button.button == SDL_BUTTON_RIGHT
                        ? MouseInput::Button::Right
                        : MouseInput::Button::None;
        in.clicks = event.button.clicks;
        x = event.button.x;
        y = event.button.y;
    }
    const auto mod = modifiers(SDL_GetModState());
    in.touch = app.replayTouch;
    in.shift = mod.shift;
    in.ctrl = mod.control;
    in.alt = mod.alt;
    std::tie(in.x, in.y) = drawablePoint(app, x, y);
    // A press the program holds (mouse reporting) is not the layout's business
    // until it is released.
    const auto result = app.layout->mouse(in, nowMs());
    setPointer(app, result.cursor);
    if (result.refused) {
        refusedNotice(app);
    }
    if (result.changed) {
        layoutChanged(app, result.empty);
    }
    if (result.openPalette) {
        openPalette(app);
    }
    if (!result.action.empty()) {
        handleAction(app, result.action);
    }
    if (result.consumed || !result.pane) {
        return;
    }
    const auto &pm = *result.pane;
    auto *pane = app.workspace->pane(pm.pane);
    if (!pane) {
        return;
    }
    auto &view = pane->view();
    auto &model = view.model();
    if (event.type == SDL_MOUSEBUTTONDOWN && mouseTracking(*pane) &&
        !mod.shift && mouseButton(event.button.button)) {
        app.mouseCaptured = true;
        app.capturePane = pm.pane;
        app.heldButton = *mouseButton(event.button.button);
        app.lastMouseCell.reset();
        sendMouse(app,
                  *pane,
                  MouseEvent::Type::Press,
                  app.heldButton,
                  pm.cell,
                  false);
    }
    else if (event.type == SDL_MOUSEBUTTONUP && app.mouseCaptured &&
             mouseButton(event.button.button)) {
        const auto button = *mouseButton(event.button.button);
        if (auto *holder = app.workspace->pane(app.capturePane)) {
            sendMouse(app,
                      *holder,
                      MouseEvent::Type::Release,
                      button,
                      pm.clamped,
                      false);
        }
        if (button == app.heldButton) {
            app.mouseCaptured = false;
            app.capturePane = 0;
            app.heldButton = MouseEvent::Button::None;
        }
    }
    else if (event.type == SDL_MOUSEMOTION && !app.dragging &&
             mouseTracking(*pane) &&
             (app.mouseCaptured || (view.modes().mouseTracking ==
                                        TerminalModes::MouseTracking::Any &&
                                    !mod.shift))) {
        if (auto *holder = app.mouseCaptured
                               ? app.workspace->pane(app.capturePane)
                               : pane) {
            sendMouse(app,
                      *holder,
                      MouseEvent::Type::Motion,
                      app.heldButton,
                      app.mouseCaptured ? pm.clamped : pm.cell,
                      true);
        }
    }
    else if (event.type == SDL_MOUSEBUTTONDOWN &&
             event.button.button == SDL_BUTTON_LEFT) {
        const auto link = pm.cell && localLinkGesture(mod.control,
                                                      mod.shift,
                                                      mouseTracking(*pane),
                                                      app.mouseCaptured)
                              ? urlAt(model, *pm.cell)
                              : std::nullopt;
        if (link) {
            app.dragging = false;
            // Keep this in the click path (transient browser user activation),
            // passing data as a string, never interpolating JavaScript source.
            EM_ASM(
                {
                    window.open(
                        UTF8ToString($0), "_blank", "noopener,noreferrer");
                },
                link->text.c_str());
        }
        else {
            app.dragging = pm.cell.has_value();
            app.dragPane = pm.pane;
            if (pm.cell) {
                selectAt(view.selection,
                         model,
                         *pm.cell,
                         mod.shift,
                         event.button.clicks);
            }
            else {
                view.selection.reset();
            }
        }
    }
    else if ((event.type == SDL_MOUSEMOTION && app.dragging) ||
             (event.type == SDL_MOUSEBUTTONUP &&
              event.button.button == SDL_BUTTON_LEFT)) {
        const bool release = event.type == SDL_MOUSEBUTTONUP;
        if (auto *dragged = app.workspace->pane(app.dragPane)) {
            auto &dragView = dragged->view();
            if (app.dragging && dragView.selection &&
                dragView.selection->valid(dragView.model())) {
                dragView.selection->extend(dragView.model(), pm.clamped);
            }
        }
        if (release) {
            app.dragging = false;
        }
    }
}

void connectPane(App &app, WebPane &pane, const std::string &sessionId, std::uint64_t resumeOffset);

void handleEvent(App &app, const SDL_Event &event) {
    auto *pane = app.focused();
    if (!pane) {
        return;
    }
    if (pane->view().state() == TerminalView::State::Replaced && pane->session &&
        !pane->session->sessionId().empty() && event.type == SDL_KEYDOWN &&
        (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER)) {
        // Another window took this terminal over: Enter takes it back, continuing from the
        // bytes already shown (the other window then shows the same notice).
        const std::string plain = pane->session->sessionId();
        const bool local = pane->attachId.rfind("local:", 0) == 0;
        const auto offset = pane->session->receivedOffset();
        connectPane(app, *pane, local ? "local:" + plain : plain, offset);
        pane->hinted = false;
        app.lastSignature.reset();
        return;
    }
    if (app.ended(*pane) && event.type == SDL_KEYDOWN &&
        (event.key.keysym.sym == SDLK_RETURN ||
         event.key.keysym.sym == SDLK_KP_ENTER)) {
        // The session is over: Enter starts again (the login form for the
        // only pane, otherwise the pane goes away). Other keys still reach
        // the layout (prefix commands); the dead program gets no input.
        if (app.workspace->paneCount() <= 1) {
            forgetLayout();
            requestReload(app);
        }
        else {
            const auto outcome =
                app.workspace->closePane(app.workspace->focusedPaneId());
            layoutChanged(app, outcome == Outcome::Empty);
        }
        return;
    }
    if ((event.type == SDL_MOUSEBUTTONDOWN ||
         event.type == SDL_MOUSEBUTTONUP) &&
        event.button.which == SDL_TOUCH_MOUSEID) {
        return; // Touches are handled as gestures below.
    }
    if (event.type == SDL_MOUSEMOTION &&
        event.motion.which == SDL_TOUCH_MOUSEID) {
        return;
    }
    if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION ||
        event.type == SDL_FINGERUP) {
        int width = 0, height = 0;
        SDL_GetWindowSize(app.window, &width, &height);
        const double x = event.tfinger.x * width;
        const double y = event.tfinger.y * height;
        const int id = int(event.tfinger.fingerId);
        const auto now = SDL_GetTicks64();
        // A finger on a divider drags it (the layout sees it as the mouse);
        // anything else is a gesture.
        if (event.type == SDL_FINGERDOWN && !app.touch.active() &&
            !app.touchDivider) {
            const auto [dx, dy] = drawablePoint(app, int(x), int(y));
            if (app.layout->nearDivider(dx, dy)) {
                app.touchDivider = true;
                app.touchFinger = id;
            }
        }
        if (app.touchDivider) {
            if (id == app.touchFinger) {
                MouseInput in;
                in.type = event.type == SDL_FINGERDOWN ? MouseInput::Type::Down
                          : event.type == SDL_FINGERMOTION
                              ? MouseInput::Type::Move
                              : MouseInput::Type::Up;
                in.button = MouseInput::Button::Left;
                in.touch = true;
                std::tie(in.x, in.y) = drawablePoint(app, int(x), int(y));
                const auto result = app.layout->mouse(in, nowMs());
                if (result.changed) {
                    layoutChanged(app, false);
                }
                if (event.type == SDL_FINGERUP) {
                    app.touchDivider = false;
                }
            }
            return;
        }
        applyTouch(app,
                   event.type == SDL_FINGERDOWN ? app.touch.down(id, x, y, now)
                   : event.type == SDL_FINGERMOTION
                       ? app.touch.move(id, x, y, now)
                       : app.touch.up(id, now));
        return;
    }
    if (event.type == SDL_TEXTINPUT) {
        const auto mod = modifiers(SDL_GetModState());
        if (app.skipText) {
            app.skipText = false;
        }
        else if (!mod.control) {
            handleText(app, event.text.text, mod.alt);
        }
    }
    else if (event.type == SDL_KEYDOWN) {
        handleKey(app, event.key);
    }
    else if (event.type == SDL_MOUSEBUTTONDOWN ||
             event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEMOTION) {
        handleMouse(app, event);
    }
    else if (event.type == SDL_WINDOWEVENT &&
             (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
              event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
        const bool gained = event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
        if (!gained) {
            applyTouch(app, app.touch.cancel());
            app.dragging = false;
            app.mouseCaptured = false;
            app.heldButton = MouseEvent::Button::None;
        }
        sendTo(*pane, InputEncoder::focus(gained, pane->view().modes()));
    }
    else if (event.type == SDL_MOUSEWHEEL) {
        handleWheel(app, event.wheel);
    }
}

// Rebuild the renderer with the pending font size; the next frame notices the
// new cell size and resizes the panes and their links as for a window resize.
// Tell the page the cell size (device pixels) so it can size the window to
// whole cells; see snapWindow() in login.js.
void publishCellSize(const App &app) {
    if (!app.backend) {
        return;
    }
    EM_ASM(
        {
            if (window.mattermCell) {
                window.mattermCell($0, $1, $2);
            }
        },
        app.backend->cellWidth(),
        app.backend->cellHeight(),
        SdlBackend::scrollbarStrip());
}

void changeFont(App &app) {
    const int wanted = app.pendingFontCss;
    try {
        app.backend->setPixelSize(fontPixels(wanted));
        app.fontCss = wanted;
        saveFontSize(app.fontCss);
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "font change failed: %s\n", error.what());
    }
    app.pendingFontCss = app.fontCss;
    app.lastSignature.reset();
    app.touchRows = 0;
    publishCellSize(app);
}

// ---- panes ----

TerminalView::Config viewConfig(const App &app) {
    TerminalView::Config config;
    config.columns = std::size_t(std::max(1, app.backend->columns(true)));
    config.rows = std::size_t(std::max(1, app.backend->rows()));
    config.glide = app.glideConfig;
    config.snapScrollToRows = !queryParam("snap").empty(); // ?snap=1: rest on whole rows
    config.replacedNotice = "This terminal is controlled from another window. Press Enter to take it back";
    return config;
}

// Connects a pane to the server: a new terminal when `sessionId` is empty,
// otherwise attaching to that terminal (taking it over from any other window).
void connectPane(App &app,
                 WebPane &pane,
                 const std::string &sessionId,
                 std::uint64_t resumeOffset) {
    pane.terminal.detach();
    pane.session.reset(); // Before the channel it listens to.
    pane.channel.reset();
    pane.link = nullptr;
    pane.requested = sessionId;
    pane.attachId = sessionId;
    DBG("connectPane url=%s session=%s", app.url.c_str(), sessionId.c_str());
    try {
        pane.channel = std::make_unique<WebSocketChannel>(app.url);
        ClientSession::Config config;
        config.clock = clockNow;
        pane.terminal.configure(config, sessionId, resumeOffset);
        pane.session = std::make_unique<ClientSession>(*pane.channel, config);
        pane.link = pane.session.get();
        auto *session = pane.session.get();
        pane.terminal.attach(*session,
                             [session] { return session->errorCode(); });
        DBG("connectPane: channel+session created");
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "link failed: %s\n", error.what());
        pane.terminal.linkFailed("cannot open the connection");
    }
}

// The pretend system (docs/FAKEOS.md), the page's mode without a server.
void startDemoPane(WebPane &pane) {
    pane.terminal.detach();
    ScriptedLink::Config config;
    config.clock = clockNow;
    config.columns = int(pane.view().model().columns());
    config.rows = int(pane.view().model().rows());
    fakeos::Config pretend;
    pretend.columns = config.columns;
    pretend.rows = config.rows;
    // ?nointro skips the intro animation.
    pretend.intro =
        EM_ASM_INT({
            return new URLSearchParams(location.search).has("nointro") ? 0 : 1;
        }) != 0;
    // ?cmd=... is typed at the first prompt (printable text only).
    for (const char c : queryParam("cmd").substr(0, 200)) {
        if ((unsigned char)c >= 0x20 && c != 0x7f) {
            pretend.startCommand += c;
        }
    }
    // Program output appears at a pace (any key shows the rest); ?noreveal
    // prints it at once.
    pretend.revealCharsPerSecond = EM_ASM_INT({
        return new URLSearchParams(location.search).has("noreveal") ? 0 : 1250;
    });
    // Pips: off unless ?sound=1; the `sound on` command turns them on, and
    // login.js plays them with WebAudio after a user gesture.
    pretend.sound =
        EM_ASM_INT({ return window.mattermSoundWanted ? 1 : 0; }) != 0;
    pretend.onBeep = [](const fakeos::Beep &beep) {
        EM_ASM(
            {
                if (window.mattermBeep) {
                    window.mattermBeep($0, $1, $2);
                }
            },
            beep.frequency,
            double(beep.durationMs),
            beep.volume);
    };
    pretend.seed = std::uint64_t(EM_ASM_DOUBLE({ return Date.now(); }));
    pretend.unixTime =
        std::int64_t(EM_ASM_DOUBLE({ return Math.floor(Date.now() / 1000); }));
    // ?chat=wss://host/path turns the `chat` command on (it does not exist
    // otherwise). It still asks before it connects and names the server.
    // login.js opens the WebSocket (window.mattermChat).
    {
        // The demo's own chat server unless ?chat= names another one
        // (?chat=off turns the command off). wss://: a page served over https
        // cannot open a plain ws:// socket.
        std::string chat = queryParam("chat");
        if (chat.empty()) {
            chat = "wss://xn--laserskld-67a.se/matterm";
        }
        else if (chat == "off") {
            chat.clear();
        }
        const auto scheme = chat.rfind("wss://", 0) == 0  ? 6u
                            : chat.rfind("ws://", 0) == 0 ? 5u
                                                          : 0u;
        bool plain = scheme != 0 && chat.size() > scheme && chat.size() <= 200;
        for (const char c : chat) {
            plain = plain && (unsigned char)c > 0x20 && c != 0x7f;
        }
        if (plain) {
            EM_ASM({ window.mattermChatUrl = UTF8ToString($0); }, chat.c_str());
            pretend.chat.server =
                chat.substr(scheme, chat.find('/', scheme) - scheme);
            pretend.chat.open = [] { EM_ASM({ window.mattermChat.open(); }); };
            pretend.chat.send = [](const std::string &line) {
                EM_ASM({ window.mattermChat.send(UTF8ToString($0)); },
                       line.c_str());
            };
            pretend.chat.close = [] {
                EM_ASM({ window.mattermChat.close(); });
            };
        }
    }
    auto program = std::make_unique<fakeos::ScriptedProgram>(pretend);
    gChat = program.get();
    pane.demo =
        std::make_unique<ScriptedLink>(config, Script(), std::move(program));
    pane.link = pane.demo.get();
    pane.terminal.attach(*pane.demo);
}

std::unique_ptr<WebPane> makePane(App &app) {
    auto pane = std::make_unique<WebPane>(viewConfig(app));
    auto *raw = pane.get();
    pane->terminal.setChanged([raw] { raw->changed = true; });
    if (app.url.empty()) {
        startDemoPane(*pane);
    }
    else {
        std::string id = std::move(app.pendingSession);
        app.pendingSession.clear();
        if (LayoutSnapshot::validId(id) && isLocal(id)) {
            id = "local:" + id; // A reload of a tab that holds a terminal taken from the laptop.
        }
        connectPane(app, *pane, std::move(id), 0);
    }
    return pane;
}

// ---- remembered layout (sessionStorage) ----
// The tabs, splits and terminal ids of this window, so that a reload attaches
// to the same terminals again. Ids only (docs: layoutsnapshot.h); the stored
// text is untrusted and parsed strictly.

// The gutter between panes: darker than the scrollbar thumb (110,122,142) so
// the left pane's scrollbar, drawn in the near half, stands out; the track is
// darker still.
constexpr Rgb8 kDividerColor{48, 53, 62};

constexpr const char *kLayoutKey = "matterm.layout";

// The id the server keeps a pane's terminal under; else the one it was asked
// for (so a refused attach is still remembered); else none yet.
std::string paneSessionId(const WebPane &pane) {
    if (pane.session &&
        (pane.session->negotiatedFeatures() & protocol::featureReplay) != 0 &&
        LayoutSnapshot::validId(pane.session->sessionId())) {
        return pane.session->sessionId();
    }
    return LayoutSnapshot::validId(pane.requested) ? pane.requested
                                                   : std::string();
}

std::optional<LayoutSnapshot> currentLayout(const App &app) {
    bool complete = true;
    using NodePtr = std::unique_ptr<SnapshotNode>;
    LayoutSnapshot snapshot;
    snapshot.tabs = app.workspace->foldTabs<NodePtr>(
        [&complete](PaneId, const WebPane &pane) {
            auto id = paneSessionId(pane);
            complete = complete && !id.empty();
            return SnapshotNode::pane(std::move(id));
        },
        [](SplitDirection direction, double ratio, NodePtr a, NodePtr b) {
            const int permille =
                std::clamp(int(std::lround(ratio * 1000)), 1, 999);
            return SnapshotNode::split(
                direction, permille, std::move(a), std::move(b));
        });
    snapshot.active = app.workspace->activeIndex();
    if (!complete || !snapshot.valid()) {
        return std::nullopt;
    }
    return snapshot;
}

// Nothing left to come back to (the last terminal ended or was closed).
void forgetLayout() {
    EM_ASM(
        {
            try {
                sessionStorage.removeItem(UTF8ToString($0));
            }
            catch (e) {
            }
        },
        kLayoutKey);
    if (gApp) {
        gApp->savedLayout.clear();
        gApp->leaving = true; // The page reloads; do not write it back.
    }
}

// Writes the layout when it changed and every pane has its id.
void saveLayout(App &app) {
    if (app.url.empty() || app.leaving) {
        return;
    }
    const auto snapshot = currentLayout(app);
    if (!snapshot) {
        return;
    }
    const auto text = snapshot->serialize();
    if (text == app.savedLayout) {
        return;
    }
    app.savedLayout = text;
    EM_ASM(
        {
            try {
                sessionStorage.setItem(UTF8ToString($0), UTF8ToString($1));
            }
            catch (e) {
            }
        },
        kLayoutKey,
        text.c_str());
}

std::optional<LayoutSnapshot> storedLayout() {
    char *value = static_cast<char *>(EM_ASM_PTR(
        {
            let v = "";
            try {
                v = sessionStorage.getItem(UTF8ToString($0)) || "";
            }
            catch (e) {
            }
            const n = lengthBytesUTF8(v) + 1;
            const p = _malloc(n);
            stringToUTF8(v, p, n);
            return p;
        },
        kLayoutKey));
    const std::string text = value;
    std::free(value);
    return LayoutSnapshot::parse(text);
}

const std::string &firstSession(const SnapshotNode &node) {
    return node.leaf ? node.session : firstSession(*node.first);
}

// Rebuilds a tab's splits below the pane `pane` (the tab's first, which
// already shows the first leaf of `node`).
void restoreNode(App &app, const SnapshotNode &node, PaneId pane) {
    if (node.leaf) {
        return;
    }
    app.workspace->focusPane(pane);
    app.pendingSession = firstSession(*node.second);
    const auto outcome = app.workspace->apply(
        node.direction == SplitDirection::SideBySide ? Command::SplitSideBySide
                                                     : Command::SplitStacked);
    app.pendingSession.clear();
    // The new pane has the focus; its split sits above it.
    const auto created =
        outcome == Outcome::Done ? app.workspace->focusedPaneId() : PaneId(0);
    if (created != 0) {
        app.workspace->setParentRatio(created, node.permille / 1000.0);
    }
    restoreNode(app, *node.first, pane);
    if (created != 0) {
        restoreNode(app, *node.second, created);
    }
}

// True when at least one tab came back.
bool restoreLayout(App &app, const LayoutSnapshot &snapshot) {
    for (const auto &tab : snapshot.tabs) {
        app.pendingSession = firstSession(*tab);
        const auto outcome = app.workspace->apply(Command::NewTab);
        app.pendingSession.clear();
        app.layout->setWindow(app.layout->window()); // The strip may appear.
        if (outcome != Outcome::Done) {
            continue;
        }
        restoreNode(app, *tab, app.workspace->focusedPaneId());
    }
    app.workspace->select(snapshot.active);
    return app.workspace->tabCount() > 0;
}

// ---- frame ----

void showTitle(App &app, const std::string &wanted) {
    if (wanted != app.shownTitle) {
        app.shownTitle = wanted;
        EM_ASM({ document.title = UTF8ToString($0); }, wanted.c_str());
    }
}

PixelBox box(const PixelRect &r) {
    return PixelBox{r.x, r.y, r.w, r.h};
}

void frame(void *data) {
    auto &app = *static_cast<App *>(data);
    SDL_Event event;
    constexpr Uint64 activeMs = 500, idleMs = 100;
    const auto start = SDL_GetTicks64();
    // Out of view (scrolled away in an embedding page, or a hidden tab): no
    // frames until the page says it is visible again. Asked a few times a
    // second.
    if (start - app.pauseChecked >= 200) {
        app.pauseChecked = start;
        app.paused = EM_ASM_INT({ return window.mattermPaused ? 1 : 0; }) != 0;
    }
    if (app.paused) {
        return;
    }
    while (SDL_PollEvent(&event)) {
        app.activeUntil = start + activeMs;
        handleEvent(app, event);
    }
    if (const auto held = app.touch.tick(SDL_GetTicks64()); !held.empty()) {
        app.activeUntil = start + activeMs;
        applyTouch(app, held);
    }
    if (const auto timeout = app.layout->tick(nowMs()); timeout.forwardPrefix) {
        send(app, prefixBytes(app)); // The prefix expired: the program gets it.
    }
    if (app.layout->prefixWaiting() != app.prefixShown) {
        app.prefixShown = app.layout->prefixWaiting();
        EM_ASM(
            {
                const b = document.querySelector("#keybar [data-prefix]");
                if (b) {
                    b.setAttribute("aria-pressed", $0 ? "true" : "false");
                }
            },
            app.prefixShown ? 1 : 0);
    }
    if (app.pendingFontCss != app.fontCss) {
        changeFont(app);
    }
    // A finger on the screen keeps the loop awake (for the long press).
    if (app.touch.active()) {
        app.activeUntil = std::max(app.activeUntil, start + activeMs);
    }
    if (start > app.activeUntil && start - app.lastFull < idleMs &&
        app.lastSignature) {
        return; // Idle: nothing happened and the next check is not due yet.
    }
    app.lastFull = start;
    // Closed panes: wait for the server's confirmation, then let go.
    for (auto it = app.closing.begin(); it != app.closing.end();) {
        it->session->tick();
        const auto state = it->session->state();
        if (state == ClientSession::State::Exited ||
            state == ClientSession::State::Failed || it->session->closeCode() ||
            start >= it->deadline) {
            it = app.closing.erase(it);
        }
        else {
            ++it;
        }
    }
    if (app.reloadDeadline != 0 &&
        (app.closing.empty() || start >= app.reloadDeadline)) {
        app.reloadDeadline = 0;
        EM_ASM({ location.reload(); });
    }
    // Every pane runs, shown or not: links are served and output is parsed.
    pollLocal(app);
    std::vector<PaneId> hiddenChanged;
    app.workspace->forEachPane([&](PaneId id, WebPane &pane, bool visible) {
        if (pane.session) {
            pane.session->tick();
        }
        if (pane.demo) {
            pane.demo->tick();
        }
        if (pane.session && !app.warnedVolatile && !app.url.empty() &&
            pane.session->state() == ClientSession::State::Running &&
            (pane.session->negotiatedFeatures() & protocol::featureReplay) ==
                0) {
            app.warnedVolatile = true;
            pane.view().note(
                "terminals are not kept on this server: a reload starts "
                "fresh");
        }
        if (app.ended(pane) && !pane.hinted &&
            pane.view().state() == TerminalView::State::Replaced) {
            pane.hinted = true; // The notice already says what Enter does.
        }
        if (app.ended(pane) && !pane.hinted) {
            pane.hinted = true;
            DBG("pane %u ended: state=%d exit=%d",
                unsigned(id),
                int(pane.view().state()),
                pane.view().exitCode());
            pane.view().note(app.workspace->paneCount() <= 1
                                 ? "Press Enter to log in again"
                                 : "Press Enter to close this pane");
        }
        if (pane.changed) {
            pane.changed = false;
            if (!visible) {
                hiddenChanged.push_back(id);
            }
        }
    });
    for (const auto id : hiddenChanged) {
        app.workspace->noteActivity(id);
    }
    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSize(app.window, &windowWidth, &windowHeight);
    // The backing store can change without SDL's window size changing (the
    // framed/full-screen toggle, a pixel-ratio change); it is cleared then.
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetRendererOutputSize(
        SDL_GetRenderer(app.window), &drawableWidth, &drawableHeight);
    WindowMetrics metrics;
    metrics.width = drawableWidth;
    metrics.height = drawableHeight;
    metrics.cellWidth = app.backend->cellWidth();
    metrics.cellHeight = app.backend->cellHeight();
    app.layout->setWindow(metrics);
    app.layout->sync(nowMs());
    const auto focusChange = app.layout->takeFocusChange();
    for (const auto &[id, gained] : {std::pair{focusChange.lost, false},
                                     std::pair{focusChange.gained, true}}) {
        if (auto *pane = app.workspace->pane(id)) {
            if (id != 0 && (SDL_GetWindowFlags(app.window) &
                            SDL_WINDOW_INPUT_FOCUS) != 0) {
                sendTo(*pane,
                       InputEncoder::focus(gained, pane->view().modes()));
            }
        }
    }
    saveLayout(app);
    const auto plan = app.layout->plan();
    if (gDebug) {
        static std::string lastPlan;
        std::string text =
            "tabs=" + std::to_string(app.workspace->tabCount()) +
            " panes=" + std::to_string(app.workspace->paneCount()) +
            " strip=" + std::to_string(plan.stripRect.h) +
            " focus=" + std::to_string(plan.focus) +
            " layers=" + std::to_string(plan.layers.size());
        for (const auto &p : plan.panes) {
            text += " [pane " + std::to_string(p.id) + " " +
                    std::to_string(p.rect.x) + "," + std::to_string(p.rect.y) +
                    " " + std::to_string(p.rect.w) + "x" +
                    std::to_string(p.rect.h) + " cells " +
                    std::to_string(p.columns) + "x" + std::to_string(p.rows);
            if (auto *w = app.workspace->pane(p.id)) {
                text += " state=" + std::to_string(int(w->view().state()));
            }
            text += "]";
        }
        if (text != lastPlan) {
            lastPlan = text;
            DBG("plan %s", text.c_str());
        }
    }
    const auto now = SDL_GetTicks64();
    const double dt = double(now - app.last) / 1000;
    app.last = now;
    const bool windowFocus =
        (SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS) != 0;

    struct Drawn {
        WebPane *pane = nullptr;
        RenderFrame frame;
        PanePlan plan;
        bool unblinkedCursorVisible = false;
    };
    std::vector<Drawn> drawn;
    drawn.reserve(plan.panes.size());
    std::optional<UrlMatch> link;
    for (const auto &placed : plan.panes) {
        auto *pane = app.workspace->pane(placed.id);
        if (!pane) {
            continue;
        }
        auto &view = pane->view();
        auto &model = view.model();
        model.advanceViewport(dt);
        auto rendered = model.frame(app.backend->cellHeight());
        rendered.palette = &view.palette();
        if (view.selection) {
            if (view.selection->valid(model)) {
                view.selection->apply(model, rendered);
            }
            else {
                view.selection.reset();
                if (app.dragPane == placed.id) {
                    app.dragging = false;
                }
            }
        }
        if (placed.focused && !app.ended(*pane)) {
            // The link under the pointer is previewed in the focused pane.
            link = hoveredLink(app, *pane, placed);
            if (link) {
                link->underline(model, rendered);
            }
        }
        rendered.textBlinkVisible = (now / 500) % 2 == 0;
        // Before blink masking: the glide follows the true cell, not the blink.
        view.glide().update(rendered.cursor.column,
                            rendered.cursor.row,
                            rendered.cursor.visible,
                            double(now) / 1000);
        view.glide().apply(rendered.cursor);
        rendered.cursor.focused = windowFocus && placed.focused;
        Drawn d;
        d.pane = pane;
        d.plan = placed;
        d.unblinkedCursorVisible = rendered.cursor.visible;
        // An inactive pane or window shows a steady hollow cursor.
        rendered.cursor.visible =
            rendered.cursor.visible &&
            (!rendered.cursor.focused || !rendered.cursor.blink ||
             (now / 500) % 2 == 0);
        d.frame = rendered;
        drawn.push_back(std::move(d));
    }
    showTitle(app,
              link ? link->text
              : app.focused() && !app.focused()->view().title().empty()
                  ? app.focused()->view().title()
                  : app.title);

    // The tab strip is one row of cells.
    std::optional<RenderFrame> stripFrame;
    if (plan.stripRect.h > 0 && !plan.strip.cells.empty()) {
        const auto columns = plan.strip.cells.size();
        if (!app.stripCells || app.stripCells->columns() != columns) {
            app.stripCells = std::make_unique<CellBuffer>(columns, 1);
        }
        for (std::size_t i = 0; i < columns; ++i) {
            app.stripCells->at(i, 0) = plan.strip.cells[i];
        }
        RenderFrame strip;
        strip.cells = app.stripCells->view();
        strip.palette = &app.chromePalette;
        stripFrame = strip;
    }

    // Overlays (the command palette): cell grids over the panes.
    std::vector<RenderFrame> layerFrames;
    layerFrames.reserve(plan.layers.size());
    for (const auto &layer : plan.layers) {
        RenderFrame f;
        f.cells = layer.cells->view();
        f.cursor = layer.cursor;
        f.cursor.focused = true;
        f.palette = &app.chromePalette;
        layerFrames.push_back(f);
    }

    // An unchanged window leaves the canvas as it is; the window size is part
    // of the signature because a resized canvas must be redrawn.
    std::vector<PaneSignatureInput> inputs;
    if (stripFrame) {
        inputs.push_back(
            {&*stripFrame, &app.chromePalette, plan.stripRect, PixelRect{}});
    }
    for (const auto &d : drawn) {
        inputs.push_back({&d.frame,
                          &d.pane->view().palette(),
                          d.plan.rect,
                          d.plan.scrollbar});
    }
    for (std::size_t i = 0; i < layerFrames.size(); ++i) {
        inputs.push_back({&layerFrames[i],
                          &app.chromePalette,
                          plan.layers[i].rect,
                          PixelRect{}});
    }
    FrameSignature signature;
    signature.add(
        windowSignature(inputs, plan.dividers, windowWidth, windowHeight));
    signature.add(drawableWidth);
    signature.add(drawableHeight);
    signature.add(plan.focus);
    if (app.lastSignature && *app.lastSignature == signature.value()) {
        return;
    }
    app.lastSignature = signature.value();
    {
        // Blinking alone must not keep the loop at full speed.
        std::vector<RenderFrame> steady;
        steady.reserve(drawn.size());
        std::vector<PaneSignatureInput> steadyInputs;
        if (stripFrame) {
            steadyInputs.push_back({&*stripFrame,
                                    &app.chromePalette,
                                    plan.stripRect,
                                    PixelRect{}});
        }
        for (const auto &d : drawn) {
            auto copy = d.frame;
            copy.textBlinkVisible = true;
            copy.cursor.visible = d.unblinkedCursorVisible;
            steady.push_back(copy);
        }
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            steadyInputs.push_back({&steady[i],
                                    &drawn[i].pane->view().palette(),
                                    drawn[i].plan.rect,
                                    drawn[i].plan.scrollbar});
        }
        for (std::size_t i = 0; i < layerFrames.size(); ++i) {
            steadyInputs.push_back({&layerFrames[i],
                                    &app.chromePalette,
                                    plan.layers[i].rect,
                                    PixelRect{}});
        }
        const auto stable = windowSignature(
            steadyInputs, plan.dividers, windowWidth, windowHeight);
        if (stable != app.lastStable) {
            app.lastStable = stable;
            app.activeUntil = now + activeMs;
        }
    }
    app.backend->beginFrame(app.chromePalette.background);
    // Gutters first: a pane draws its scrollbar in the half next to it.
    for (const auto &divider : plan.dividers) {
        app.backend->drawRect(box(divider.rect), kDividerColor);
    }
    if (stripFrame) {
        app.backend->render(*stripFrame, PaneArea{box(plan.stripRect), {}});
    }
    for (const auto &d : drawn) {
        app.backend->render(d.frame,
                            PaneArea{box(d.plan.rect), box(d.plan.scrollbar)});
    }
    for (std::size_t i = 0; i < layerFrames.size(); ++i) {
        app.backend->render(layerFrames[i],
                            PaneArea{box(plan.layers[i].rect), {}});
    }
    app.backend->present();
}
} // namespace

// The chat connection (login.js, window.mattermChat) reports: kind 0 opened, 1
// a text frame (`data`), 2 closed. Handled at the next frame.
extern "C" EMSCRIPTEN_KEEPALIVE void matterm_chat_event(int kind,
                                                        const char *data) {
    if (gChat && gApp) {
        gChat->chatEvent(kind, data ? data : "");
        gApp->activeUntil = SDL_GetTicks64() + 500;
    }
}

// Called by login.js's paste listener with the clipboard text.
extern "C" EMSCRIPTEN_KEEPALIVE void matterm_paste(const char *text) {
    if (gApp && text) {
        if (auto *pane = gApp->focused()) {
            sendTo(*pane, InputEncoder::paste(text, pane->view().modes()));
        }
    }
}

// The extra-keys bar's prefix button: arms the layout prefix (Ctrl+B), then the
// next key or tap (a command key, text from the soft keyboard) is its second
// key.
extern "C" EMSCRIPTEN_KEEPALIVE void matterm_prefix() {
    if (gApp && gApp->layoutKeys()) {
        layoutKey(*gApp, gApp->layout->config().prefix.prefix);
        gApp->activeUntil = SDL_GetTicks64() + 500;
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
    if (gApp->layoutKeys()) {
        if (control && std::strlen(utf8) == 1) {
            if (layoutKey(*gApp,
                          KeyChord{foldKey(utf8[0]), true, false, alt})) {
                return;
            }
        }
        else if (!control) {
            handleText(*gApp, utf8, alt); // The layout first, then the program.
            return;
        }
    }
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
    auto *pane = gApp->focused();
    if (!pane) {
        return;
    }
    if (gApp->layoutKeys()) {
        static const std::pair<const char *, int> chords[] = {
            {"Enter", keys::Enter},
            {"Backspace", keys::Backspace},
            {"Tab", keys::Tab},
            {"Escape", keys::Escape},
            {"ArrowUp", keys::Up},
            {"ArrowDown", keys::Down},
            {"ArrowLeft", keys::Left},
            {"ArrowRight", keys::Right},
            {"PageUp", keys::PageUp},
            {"PageDown", keys::PageDown}};
        for (const auto &[chordName, code] : chords) {
            if (std::strcmp(chordName, name) == 0 &&
                layoutKey(*gApp,
                          KeyChord{code,
                                   (mods & 1) != 0,
                                   (mods & 4) != 0,
                                   (mods & 2) != 0})) {
                return;
            }
        }
    }
    for (const auto &[keyName, key] : keys) {
        if (std::strcmp(keyName, name) == 0) {
            InputEncoder::Modifiers modifiers;
            modifiers.control = (mods & 1) != 0;
            modifiers.alt = (mods & 2) != 0;
            modifiers.shift = (mods & 4) != 0;
            sendTo(*pane,
                   InputEncoder::key(key, modifiers, pane->view().modes()));
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
    gDebug =
        EM_ASM_INT({
            return new URLSearchParams(location.search).has("debug") ? 1 : 0;
        }) != 0;
    // Touches arrive as fingers only; taps are replayed as clicks by
    // touchTap and drags are gestures, so SDL must not also fake a mouse.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    app.fontCss = app.pendingFontCss = loadFontSize();
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
            *app.window, "/assets/font.ttf", fontPixels(app.fontCss));
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "backend failed: %s\n", error.what());
        return 1;
    }
    app.backend->setReserveScrollbarStrip(true);
    publishCellSize(app);
    app.url = wsUrl();
    // ?smooth glides the cursor between positions; ?smooth=300 sets the glide
    // time in milliseconds (default 80). It is off for a real terminal session
    // and on in the static demo (no server URL); ?smooth=0 turns it off there.
    const int smoothMs = EM_ASM_INT(
        {
            const p = new URLSearchParams(location.search);
            if (!p.has("smooth")) {
                return $0 ? 80 : 0;
            }
            const v = parseInt(p.get("smooth"), 10);
            return v == 0 ? 0 : (v > 0 ? v : 80);
        },
        app.url.empty() ? 1 : 0);
    app.glideConfig.enabled = smoothMs > 0;
    app.glideConfig.durationSeconds = smoothMs / 1000.0;
    app.workspace =
        std::make_unique<Workspace<WebPane>>([] { return makePane(*gApp); });
    app.workspace->setMaxPanes(
        LayoutSnapshot::kMaxPanes); // 16: the login's leases.
    ViewConfig layoutConfig;
    layoutConfig.palette = app.chromePalette;
    layoutConfig.paletteButton =
        !app.url.empty(); // A way in without remembering keys.
    layoutConfig.showStripWithOneTab = false; // Hidden until there is a choice.
    layoutConfig.compactBelowColumns = 60;    // A phone: one pane at a time.
    layoutConfig.prefix.timeoutMs = 4000;     // Time to reach for the keyboard.
    layoutConfig.desktopShortcuts = false;    // The browser keeps Ctrl+T/N/W.
    layoutConfig.prefixKeys = !app.url.empty(); // Off in the static demo.
    layoutConfig.scrollbarStrip = SdlBackend::scrollbarStrip();
    layoutConfig.dividerThickness =
        16; // Half of it holds the left pane's scrollbar.
    app.layout =
        std::make_unique<WorkspaceView<WebPane>>(*app.workspace, layoutConfig);
    if (app.layoutKeys()) {
        // The extra-keys bar (touch) gets its prefix button; the static demo
        // has no tabs, so it stays hidden there.
        EM_ASM({
            const b = document.querySelector("#keybar [data-prefix]");
            if (b) {
                b.hidden = false;
            }
        });
    }
    WindowMetrics metrics;
    SDL_GetRendererOutputSize(
        SDL_GetRenderer(app.window), &metrics.width, &metrics.height);
    metrics.cellWidth = app.backend->cellWidth();
    metrics.cellHeight = app.backend->cellHeight();
    app.layout->setWindow(metrics);
    // After a reload: the tabs and splits this window had, attached to their
    // terminals again; otherwise one new terminal.
    if (!app.url.empty()) {
        if (const auto stored = storedLayout()) {
            restoreLayout(app, *stored);
        }
    }
    if (app.workspace->tabCount() == 0) {
        app.workspace->apply(Command::NewTab);
    }
    if (!app.focused()) {
        std::fprintf(stderr, "no terminal\n");
        return 1;
    }
    app.layout->sync(nowMs(), true);
    askLocalSessions(app); // Whether the server offers the laptop's shared terminals.
    if (app.layoutKeys()) {
        // The extra-keys bar gets its tabs/panes prefix button (server mode).
        EM_ASM({
            const b = document.querySelector("#keybar [data-prefix]");
            if (b) {
                b.hidden = false;
            }
        });
    }
    SDL_StartTextInput();
    app.last = SDL_GetTicks64();
    emscripten_set_main_loop_arg(frame, &app, 0, 1);
    return 0;
}
