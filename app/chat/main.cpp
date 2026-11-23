// mattrix-chat: the Mattrix chat in the terminal it was started from. It
// connects to a chatd server (docs/CHAT.md): plain TCP lines or a WebSocket,
// both without TLS here (TLS is the reverse proxy's job for now), so it is
// meant for local testing and for a loopback tunnel.

#include "chatclient.h"
#include "chatview.h"
#include "keys.h"
#include "tty/posix/terminal.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program
        << " (--tcp HOST:PORT | --ws HOST:PORT) [OPTIONS]\n\n"
        << "The Mattrix chat in your terminal: live chat, your draft is sent "
           "as you\n"
           "type. It connects to the server you name and shows it your IP "
           "address.\n\n"
           "  --help, -h        Show this help.\n"
           "  --tcp HOST:PORT   Plain TCP lines (a chatd started with "
           "--test-tcp).\n"
           "  --ws HOST:PORT    WebSocket without TLS (ws://). Public servers "
           "use wss\n"
           "                    behind a proxy; that needs TLS here, which is "
           "not built.\n"
           "  --origin URL      The Origin header for --ws (default "
           "http://localhost).\n"
           "  --name NAME       Your name (2-16 of A-Z a-z 0-9 _ -; default "
           "guest-NNNN).\n"
           "  --yes             Do not ask before connecting.\n"
           "  --no-animations   No pulsing first line.\n\n"
           "In the chat: Enter sends at once, a pause of five seconds sends "
           "too. Ctrl-C\n"
           "leaves. /help lists the commands.\n";
}

bool splitHostPort(const std::string &text,
                   std::string &host,
                   std::string &port) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
    return true;
}

// One connection to the server, whichever way it is made.
class Channel {
public:
    virtual ~Channel() = default;
    virtual void open() = 0;
    virtual void send(const std::string &line) = 0;
    virtual void close() = 0;
    std::function<void()> onOpened;
    std::function<void(const std::string &)> onLine;
    std::function<void()> onClosed;
};

class TcpChannel final : public Channel {
public:
    TcpChannel(net::io_context &io, std::string host, std::string port)
        : resolver_(io)
        , socket_(io)
        , host_(std::move(host))
        , port_(std::move(port)) {}
    void open() override {
        resolver_.async_resolve(
            host_, port_, [this](beast::error_code ec, auto results) {
                if (ec) {
                    return failed();
                }
                net::async_connect(socket_,
                                   results,
                                   [this](beast::error_code e, const auto &) {
                                       if (e) {
                                           return failed();
                                       }
                                       opened_ = true;
                                       onOpened();
                                       read();
                                   });
            });
    }
    void send(const std::string &line) override {
        if (!opened_ || closed_) {
            return;
        }
        out_.push_back(line + "\n");
        if (!writing_) {
            write();
        }
    }
    void close() override {
        closed_ = true;
        beast::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

private:
    void failed() {
        if (!closed_) {
            closed_ = true;
            onClosed();
        }
    }
    void write() {
        writing_ = true;
        net::async_write(socket_,
                         net::buffer(out_.front()),
                         [this](beast::error_code ec, std::size_t) {
                             out_.pop_front();
                             writing_ = false;
                             if (ec) {
                                 return failed();
                             }
                             if (!out_.empty()) {
                                 write();
                             }
                         });
    }
    void read() {
        net::async_read_until(
            socket_, buffer_, '\n', [this](beast::error_code ec, std::size_t) {
                if (ec || closed_) {
                    return failed();
                }
                std::istream stream(&buffer_);
                std::string line;
                std::getline(stream, line);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                onLine(line);
                if (!closed_) {
                    read();
                }
            });
    }
    tcp::resolver resolver_;
    tcp::socket socket_;
    net::streambuf buffer_{2048};
    std::string host_, port_;
    std::deque<std::string> out_;
    bool opened_ = false, writing_ = false, closed_ = false;
};

class WsChannel final : public Channel {
public:
    WsChannel(net::io_context &io,
              std::string host,
              std::string port,
              std::string origin)
        : resolver_(io)
        , ws_(io)
        , host_(std::move(host))
        , port_(std::move(port))
        , origin_(std::move(origin)) {}
    void open() override {
        resolver_.async_resolve(
            host_, port_, [this](beast::error_code ec, auto results) {
                if (ec) {
                    return failed();
                }
                net::async_connect(
                    ws_.next_layer(),
                    results,
                    [this](beast::error_code e, const auto &) {
                        if (e) {
                            return failed();
                        }
                        const std::string origin = origin_;
                        ws_.set_option(ws::stream_base::decorator(
                            [origin](ws::request_type &req) {
                                req.set(beast::http::field::origin, origin);
                            }));
                        ws_.read_message_max(2048);
                        ws_.async_handshake(host_ + ":" + port_,
                                            "/",
                                            [this](beast::error_code h) {
                                                if (h) {
                                                    return failed();
                                                }
                                                opened_ = true;
                                                onOpened();
                                                read();
                                            });
                    });
            });
    }
    void send(const std::string &line) override {
        if (!opened_ || closed_) {
            return;
        }
        out_.push_back(line);
        if (!writing_) {
            write();
        }
    }
    void close() override {
        closed_ = true;
        beast::error_code ignored;
        ws_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
        ws_.next_layer().close(ignored);
    }

private:
    void failed() {
        if (!closed_) {
            closed_ = true;
            onClosed();
        }
    }
    void write() {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(out_.front()),
                        [this](beast::error_code ec, std::size_t) {
                            out_.pop_front();
                            writing_ = false;
                            if (ec) {
                                return failed();
                            }
                            if (!out_.empty()) {
                                write();
                            }
                        });
    }
    void read() {
        buffer_.consume(buffer_.size());
        ws_.async_read(buffer_, [this](beast::error_code ec, std::size_t) {
            if (ec || closed_) {
                return failed();
            }
            onLine(beast::buffers_to_string(buffer_.data()));
            if (!closed_) {
                read();
            }
        });
    }
    tcp::resolver resolver_;
    ws::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::string host_, port_, origin_;
    std::deque<std::string> out_;
    bool opened_ = false, writing_ = false, closed_ = false;
};

// The whole program: the channel, the client, the view and the keyboard.
class App {
public:
    App(net::io_context &io,
        std::unique_ptr<Channel> channel,
        const std::string &name,
        bool animations)
        : io_(io)
        , channel_(std::move(channel))
        , client_(config(name))
        , view_(viewConfig(animations))
        , timer_(io)
        , input_(io)
        , signals_(io, SIGWINCH)
        , stop_(io, SIGTERM, SIGHUP)
        , started_(Clock::now()) {}

    int run() {
        channel_->onOpened = [this] { apply(client_.opened(now())); };
        channel_->onLine = [this](const std::string &line) {
            apply(client_.receive(line, now()));
            syncInput();
        };
        channel_->onClosed = [this] {
            apply(client_.closed(now()));
            if (!everOpened_) {
                failed_ = true;
            }
            finish();
        };
        // Wrap onOpened to remember that the server was reached.
        const auto opened = channel_->onOpened;
        channel_->onOpened = [this, opened] {
            everOpened_ = true;
            opened();
        };
        tty::windowSize(STDOUT_FILENO, columns_, rows_);
        apply(client_.connect(now()));
        channel_->open();
        input_.assign(::dup(STDIN_FILENO));
        readKeys();
        watchSize();
        stop_.async_wait([this](const beast::error_code &, int) { leave(); });
        redraw();
        io_.run();
        return failed_ ? 1 : 0;
    }

private:
    static chat::ChatClient::Config config(const std::string &name) {
        chat::ChatClient::Config c;
        c.name = name;
        return c;
    }
    static chat::ViewConfig viewConfig(bool animations) {
        int columns = 80, rows = 24;
        tty::windowSize(STDOUT_FILENO, columns, rows);
        chat::ViewConfig v;
        const auto caps = tty::detectCaps();
        v.caps.truecolor = caps.truecolor;
        v.caps.colors256 = caps.colors256;
        v.caps.unicode = caps.unicode;
        v.columns = columns;
        v.rows = rows;
        v.animations = animations;
        const auto unixNow =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        v.unixMsAtZero = unixNow;
        std::time_t t = std::time(nullptr);
        std::tm local{};
        localtime_r(&t, &local);
        v.utcOffsetMinutes = int(local.tm_gmtoff / 60);
        return v;
    }
    chat::Millis now() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - started_)
            .count();
    }
    void apply(const chat::ChatClient::Out &out) {
        for (const auto &line : out.send) {
            channel_->send(line);
        }
        if (out.close) {
            channel_->close();
            finish();
        }
    }
    void redraw() {
        if (done_) {
            return;
        }
        tty::writeAll(STDOUT_FILENO, view_.render(client_, editor_, now()));
        schedule();
    }
    void schedule() {
        if (done_) {
            return;
        }
        std::optional<chat::Millis> next;
        auto consider = [&](std::optional<chat::Millis> t) {
            if (t && (!next || *t < *next)) {
                next = t;
            }
        };
        consider(client_.nextDeadline());
        consider(view_.nextFrame(now()));
        if (!next) {
            timer_.cancel();
            return;
        }
        timer_.expires_at(started_ + std::chrono::milliseconds(*next));
        timer_.async_wait([this](const beast::error_code &ec) {
            if (ec || done_) {
                return;
            }
            apply(client_.tick(now()));
            redraw();
        });
    }
    // What is in the editor goes to the client, except a command.
    void syncDraft() {
        const std::string text = editor_.text();
        chat::ChatClient::Out out;
        client_.edit(
            text.rfind('/', 0) == 0 ? std::string() : text, now(), out);
        apply(out);
    }
    // The client may have changed the draft (an acknowledged part was cut).
    void syncInput() {
        const std::string text = editor_.text();
        if (text.rfind('/', 0) != 0 &&
            client_.state() == chat::ChatClient::State::Joined &&
            client_.draft() != text) {
            editor_.set(client_.draft());
        }
        redraw();
    }
    void command(const std::string &line) {
        const auto space = line.find(' ');
        const std::string verb = line.substr(0, space);
        const std::string arg =
            space == std::string::npos ? std::string() : line.substr(space + 1);
        if (verb == "/quit") {
            leave();
        }
        else if (verb == "/nick") {
            const auto out = client_.rename(arg, now());
            if (out.send.empty()) {
                view_.note("a name is 2-16 of A-Z a-z 0-9 _ -");
            }
            apply(out);
        }
        else if (verb == "/who") {
            std::string names;
            for (const auto &n : client_.users()) {
                names += (names.empty() ? "" : ", ") + n;
            }
            view_.note("here: " + names);
        }
        else if (verb == "/help") {
            view_.note("Enter sends at once, five quiet seconds send too. "
                       "Ctrl-C leaves.");
            view_.note("/nick NAME   /who   /quit   (everything you type is "
                       "seen live)");
        }
        else {
            view_.note("unknown command (try /help)");
        }
    }
    void key(const fakeos::Key &k) {
        using T = fakeos::Key::Type;
        bool changed = false;
        switch (k.type) {
        case T::Text:
            if (k.alt && k.text == "d") {
                changed = editor_.killWordForward();
            }
            else if (k.alt && (k.text == "b" || k.text == "f")) {
                k.text == "b" ? editor_.wordLeft() : editor_.wordRight();
                redraw();
            }
            else if (!k.alt) {
                changed = editor_.insertText(k.text);
            }
            break;
        case T::Backspace:
            changed =
                k.alt || k.ctrl ? editor_.killWord() : editor_.backspace();
            break;
        case T::Delete:
            changed =
                k.alt || k.ctrl ? editor_.killWordForward() : editor_.del();
            break;
        case T::Left:
            k.alt || k.ctrl ? editor_.wordLeft() : editor_.left();
            redraw();
            break;
        case T::Right:
            k.alt || k.ctrl ? editor_.wordRight() : editor_.right();
            redraw();
            break;
        case T::Home:
            changed = editor_.home();
            redraw();
            break;
        case T::End:
            changed = editor_.end();
            redraw();
            break;
        case T::Enter: {
            const std::string text = editor_.text();
            if (client_.state() == chat::ChatClient::State::NeedName) {
                apply(client_.hello(text, now()));
                editor_.killLine();
            }
            else if (text.rfind('/', 0) == 0) {
                editor_.killLine();
                command(text);
            }
            else {
                apply(client_.submit(now()));
            }
            redraw();
            return;
        }
        case T::Ctrl:
            if (k.text == "c" || (k.text == "d" && editor_.text().empty())) {
                leave();
                return;
            }
            if (k.text == "u") {
                changed = editor_.killLine();
            }
            else if (k.text == "w") {
                changed = editor_.killWord();
            }
            else if (k.text == "a") {
                changed = editor_.home();
                redraw();
            }
            else if (k.text == "e") {
                changed = editor_.end();
                redraw();
            }
            break;
        default:
            break;
        }
        if (changed) {
            if (client_.state() == chat::ChatClient::State::NeedName) {
                redraw(); // A name being typed is not a message.
            }
            else {
                syncDraft();
                redraw();
            }
        }
    }
    void readKeys() {
        input_.async_read_some(
            net::buffer(buffer_),
            [this](const beast::error_code &ec, std::size_t n) {
                if (ec || done_) {
                    if (ec && !done_) {
                        leave();
                    }
                    return;
                }
                for (const auto &k :
                     decoder_.feed(std::string_view(buffer_, n))) {
                    if (done_) {
                        break;
                    }
                    key(k);
                }
                readKeys();
            });
    }
    void watchSize() {
        signals_.async_wait([this](const beast::error_code &ec, int) {
            if (ec || done_) {
                return;
            }
            tty::windowSize(STDOUT_FILENO, columns_, rows_);
            tty::writeAll(STDOUT_FILENO, view_.resize(columns_, rows_));
            redraw();
            watchSize();
        });
    }
    void leave() {
        if (done_) {
            return;
        }
        apply(client_.leave(now()));
        finish();
    }
    void finish() {
        if (done_) {
            return;
        }
        // Show the last things (a closing notice, the reason) before ending.
        tty::writeAll(STDOUT_FILENO, view_.render(client_, editor_, now()));
        done_ = true;
        tty::writeAll(STDOUT_FILENO, view_.finish());
        timer_.cancel();
        beast::error_code ignored;
        input_.cancel(ignored);
        signals_.cancel();
        stop_.cancel();
        channel_->close();
        // Nothing more to wait for: do not let a pending read keep the loop.
        net::post(io_, [this] { io_.stop(); });
    }

    net::io_context &io_;
    std::unique_ptr<Channel> channel_;
    chat::ChatClient client_;
    chat::ChatView view_;
    chat::ChatInput editor_;
    net::steady_timer timer_;
    net::posix::stream_descriptor input_;
    net::signal_set signals_, stop_;
    fakeos::KeyDecoder decoder_;
    Clock::time_point started_;
    char buffer_[1024];
    int columns_ = 80, rows_ = 24;
    bool done_ = false, failed_ = false, everOpened_ = false;
};

std::string randomName() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    return "guest-" + std::to_string(1000 + (t / 1000) % 9000);
}

} // namespace

int main(int argc, char **argv) {
    std::string host, port, kind, origin = "http://localhost",
                                  name = randomName();
    bool yes = false, animations = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout, argv[0]);
            return 0;
        }
        else if ((arg == "--tcp" || arg == "--ws") && i + 1 < argc) {
            kind = arg;
            if (!splitHostPort(argv[++i], host, port)) {
                std::cerr << arg << " needs HOST:PORT\n";
                return 1;
            }
        }
        else if (arg == "--origin" && i + 1 < argc) {
            origin = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc) {
            name = argv[++i];
        }
        else if (arg == "--yes") {
            yes = true;
        }
        else if (arg == "--no-animations") {
            animations = false;
        }
        else {
            usage(std::cerr, argv[0]);
            return 1;
        }
    }
    if (kind.empty()) {
        usage(std::cerr, argv[0]);
        return 1;
    }
    if (chat::checkName(name) != chat::NameError::None) {
        std::cerr
            << "--name: 2-16 of A-Z a-z 0-9 _ -, and not a reserved name\n";
        return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr
            << "mattrix-chat needs a terminal on standard input and output\n";
        return 1;
    }
    std::cout << "Live chat, draft sent as you type.\n"
              << "This connects to " << host << ":" << port << " ("
              << (kind == "--ws" ? "WebSocket" : "plain TCP")
              << ", not encrypted here). The server sees your IP address; "
                 "other users see\n"
              << "only your name. Everyone sees what you type while you type "
                 "it, and a pause\n"
              << "of 5 seconds posts it. Messages are public and kept only in "
                 "the server's\n"
              << "memory for a short time.\n";
    if (!yes) {
        std::cout << "Connect? [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "Not connecting.\n";
            return 0;
        }
    }
    try {
        net::io_context io;
        std::unique_ptr<Channel> channel;
        if (kind == "--tcp") {
            channel = std::make_unique<TcpChannel>(io, host, port);
        }
        else {
            channel = std::make_unique<WsChannel>(io, host, port, origin);
        }
        tty::RawMode raw(STDIN_FILENO);
        if (!raw.enter()) {
            std::cerr << "cannot put the terminal in raw mode\n";
            return 1;
        }
        App app(io, std::move(channel), name, animations);
        const int code = app.run();
        if (code != 0) {
            std::cerr << "mattrix-chat: could not connect to " << host << ":"
                      << port << "\n";
        }
        return code;
    }
    catch (const std::exception &error) {
        std::cerr << "mattrix-chat: " << error.what() << '\n';
        return 1;
    }
}
