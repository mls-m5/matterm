// matterm-daemon-e2e: a daemon on a temporary socket plus a few commands on stdin, for end-to-end
// tests of the sharing path (web/tools/e2e-local.js). TEST SUPPORT ONLY: it is not installed, it
// refuses a socket outside the temp directory or $MATTERM_E2E_DIR, and it never reads the
// user's real daemon socket.
//
//   matterm-daemon-e2e --socket /tmp/x/d.sock [--sessions N] [--share K] [--send TEXT] [--max-seconds S]
//
// Starts the daemon, creates N sessions (default 1), marks the first K shared (default 0), types
// TEXT + newline into each (so the screen has something to find), prints one JSON line
//   {"socket":"...","sessions":[{"id":"...","shared":true},...]}
// and then reads commands from stdin, one per line, answering one line each:
//   new            -> {"id":"..."}              share ID / unshare ID -> {"ok":true}
//   list           -> {"sessions":[...]}        quit (or EOF)         -> exits, ending all sessions
#include "control.h"
#include "daemon.h"
#include "framing.h"
#include "message.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
unsigned number(const char *text) {
    char *end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    return end && !*end && v < 1000000 ? unsigned(v) : 0;
}

class Conn {
public:
    explicit Conn(const std::string &path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Conn() {
        if (fd_ >= 0) ::close(fd_);
    }
    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;
    bool ok() const { return fd_ >= 0; }
    bool send(const std::string &payload) const {
        std::string framed;
        return mtd::encodeFrame(payload, framed) &&
               ::send(fd_, framed.data(), framed.size(), MSG_NOSIGNAL) == ssize_t(framed.size());
    }
    std::optional<std::string> frame(int timeoutMs = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (!pending_.empty()) {
                auto f = std::move(pending_.front());
                pending_.erase(pending_.begin());
                return f;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0 || fd_ < 0) return std::nullopt;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, int(left.count())) <= 0) continue;
            char buf[4096];
            const auto n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) return std::nullopt;
            if (!decoder_.feed(std::string_view(buf, std::size_t(n)), [&](std::string_view f) { pending_.emplace_back(f); }))
                return std::nullopt;
        }
    }

private:
    int fd_ = -1;
    mtd::FrameDecoder decoder_;
    std::vector<std::string> pending_;
};

std::optional<mtd::ControlReply> ask(Conn &c, const mtd::ControlRequest &request) {
    std::string bytes;
    if (!mtd::encode(request, bytes) || !c.send(bytes)) return std::nullopt;
    const auto f = c.frame();
    return f ? mtd::decodeReply(*f) : std::nullopt;
}

std::string json(const std::string &s) {
    std::string o = "\"";
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        if (static_cast<unsigned char>(ch) >= 0x20) o += ch;
    }
    return o + "\"";
}

std::string listJson(Conn &c) {
    const auto r = ask(c, mtd::ListSessions{});
    std::string out = "{\"sessions\":[";
    if (r) {
        if (const auto *l = std::get_if<mtd::SessionList>(&*r)) {
            bool first = true;
            for (const auto &s : l->sessions) {
                out += (first ? "" : ",") + std::string("{\"id\":") + json(s.id) + ",\"shared\":" +
                       (s.shared ? "true" : "false") + ",\"attached\":" + (s.attached ? "true" : "false") + "}";
                first = false;
            }
        }
    }
    return out + "]}";
}

// Types a line into a session through an ordinary terminal connection, then leaves it running.
void typeInto(const std::string &socket, const std::string &id, const std::string &text) {
    Conn c(socket);
    if (!c.ok()) return;
    protocol::Hello h;
    h.sessionId = id;
    h.features = protocol::featureReplay;
    std::string bytes;
    protocol::encode(protocol::ClientMessage{h}, bytes);
    c.send(bytes);
    for (int i = 0; i < 20; ++i) {
        const auto f = c.frame(500);
        if (!f) break;
        const auto m = protocol::decodeServer(*f);
        if (m && std::holds_alternative<protocol::Welcome>(*m.message)) break;
    }
    bytes.clear();
    protocol::encode(protocol::ClientMessage{protocol::Input{text + "\n"}}, bytes);
    c.send(bytes);
    // Give the shell a moment to print before detaching.
    for (int i = 0; i < 5; ++i) {
        if (!c.frame(200)) break;
    }
}

bool allowedSocket(const std::string &path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> roots{fs::temp_directory_path(ec)};
    if (const char *extra = std::getenv("MATTERM_E2E_DIR")) roots.emplace_back(extra);
    const auto parent = fs::weakly_canonical(fs::path(path).parent_path(), ec);
    if (ec) return false;
    for (const auto &root : roots) {
        const auto r = fs::weakly_canonical(root, ec);
        if (ec) continue;
        const auto rel = parent.lexically_relative(r);
        if (!rel.empty() && *rel.begin() != "..") return true;
    }
    return false;
}
} // namespace

int main(int argc, char **argv) {
    std::string socket;
    unsigned sessions = 1, share = 0, maxSeconds = 600;
    std::string send;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--socket") socket = value();
        else if (a == "--sessions") sessions = number(value());
        else if (a == "--share") share = number(value());
        else if (a == "--send") send = value();
        else if (a == "--max-seconds") maxSeconds = number(value());
        else {
            (void)std::fputs("usage: matterm-daemon-e2e --socket PATH [--sessions N] [--share K] [--send TEXT] [--max-seconds S]\n", stderr);
            return 2;
        }
    }
    if (socket.empty() || socket.front() != '/' || !allowedSocket(socket)) {
        (void)std::fputs("matterm-daemon-e2e: --socket must be an absolute path under the temp directory (or $MATTERM_E2E_DIR)\n", stderr);
        return 2;
    }
    auto config = mtd::defaultConfig();
    config.socketPath = socket;
    config.privateDirectory = false;
    config.idleExitSeconds = 0;
    config.process.arguments = {"/bin/sh"};
    config.build = "matterm-daemon-e2e";
    mtd::Daemon daemon(std::move(config));
    std::string error;
    if (!daemon.start(error)) {
        (void)std::fprintf(stderr, "matterm-daemon-e2e: %s\n", error.c_str());
        return 1;
    }
    std::thread server([&daemon] { daemon.run(); });
    std::thread watchdog([&daemon, maxSeconds] {
        std::this_thread::sleep_for(std::chrono::seconds(maxSeconds));
        daemon.stop();
    });
    watchdog.detach();
    Conn control(socket);
    std::string hello;
    mtd::encode(mtd::ControlRequest(mtd::ControlHello{}), hello);
    if (!control.ok() || !control.send(hello) || !control.frame()) {
        daemon.stop();
        server.join();
        return 1;
    }
    for (unsigned i = 0; i < sessions; ++i) {
        const auto r = ask(control, mtd::CreateSession{"", 100, 30});
        const auto *created = r ? std::get_if<mtd::SessionCreated>(&*r) : nullptr;
        if (!created) continue;
        if (!send.empty()) typeInto(socket, created->id, send);
        if (i < share) ask(control, mtd::SetShared{created->id, true});
    }
    std::cout << "{\"socket\":" << json(socket) << "," << listJson(control).substr(1) << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream words(line);
        std::string command, id;
        words >> command >> id;
        if (command == "quit") break;
        if (command == "list") {
            std::cout << listJson(control) << std::endl;
        }
        else if (command == "new") {
            const auto r = ask(control, mtd::CreateSession{"", 100, 30});
            const auto *created = r ? std::get_if<mtd::SessionCreated>(&*r) : nullptr;
            std::cout << (created ? "{\"id\":" + json(created->id) + "}" : std::string("{\"error\":\"create failed\"}")) << std::endl;
        }
        else if (command == "share" || command == "unshare") {
            const auto r = ask(control, mtd::SetShared{id, command == "share"});
            std::cout << (r && std::holds_alternative<mtd::SessionList>(*r) ? "{\"ok\":true}" : "{\"ok\":false}") << std::endl;
        }
        else {
            std::cout << "{\"error\":\"unknown command\"}" << std::endl;
        }
    }
    daemon.stop();
    server.join();
    return 0;
}
