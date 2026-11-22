// Security review of the sharing path (docs/DAEMON_REVIEW.md): each finding has a test here.
#include "localfixture.h"
#include "mls-unit-test/unittest.h"

#include <fstream>
#include <sstream>

namespace {
using namespace std::chrono_literals;

http::response<http::string_body> call(Fixture &f, http::verb method, const std::string &target,
                                       const std::string &cookie, const std::string &origin) {
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({net::ip::make_address("127.0.0.1"), f.port});
    http::request<http::string_body> request(method, target, 11);
    request.set(http::field::host, "localhost");
    if (!origin.empty()) request.set(http::field::origin, origin);
    if (!cookie.empty()) request.set(http::field::cookie, cookie);
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response;
}

std::string tempDir(const char *prefix = "mtrXXXXXX") {
    std::string t = (std::filesystem::temp_directory_path() / prefix).string();
    return ::mkdtemp(t.data());
}
std::string slurp(const std::string &path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
// A daemon config for direct Daemon tests.
mtd::DaemonConfig configAt(const std::string &path, bool privateDir) {
    auto c = mtd::defaultConfig();
    c.socketPath = path;
    c.privateDirectory = privateDir;
    c.process.arguments = {"/bin/sh"};
    return c;
}
// First frame the daemon sends on an exposed connection after Hello, as raw bytes.
std::optional<std::string> exposedAnswer(const std::string &socket, const std::string &id) {
    auto c = exposedClient(socket);
    if (!c) return std::nullopt;
    c->send(hello(id));
    const auto f = c->frame();
    c->atEof();
    return f;
}
} // namespace

TEST_SUIT_BEGIN(DaemonReview)

// ---- Daemon socket, directory, lock -------------------------------------------------------

TEST_CASE("A symlink planted as the lock file is not followed") {
    const auto dir = tempDir();
    const auto victim = dir + "/victim.txt";
    { std::ofstream(victim) << "precious"; }
    std::filesystem::create_symlink(victim, dir + "/d.sock.lock");
    mtd::Daemon d(configAt(dir + "/d.sock", false));
    std::string error;
    EXPECT_FALSE(d.start(error));
    EXPECT_EQ(slurp(victim), std::string("precious"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Only a stale socket is replaced, never another kind of file") {
    const auto dir = tempDir();
    { std::ofstream(dir + "/d.sock") << "not a socket"; }
    {
        mtd::Daemon d(configAt(dir + "/d.sock", false));
        std::string error;
        EXPECT_FALSE(d.start(error));
        EXPECT_EQ(slurp(dir + "/d.sock"), std::string("not a socket"));
    }
    std::filesystem::remove(dir + "/d.sock");
    std::filesystem::create_symlink("/etc/passwd", dir + "/d.sock");
    {
        mtd::Daemon d(configAt(dir + "/d.sock", false));
        std::string error;
        EXPECT_FALSE(d.start(error));
        EXPECT(std::filesystem::is_symlink(dir + "/d.sock"));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("A private directory must be a real 0700 directory of ours, created 0700") {
    const auto dir = tempDir();
    {
        // Missing: created with 0700.
        mtd::Daemon d(configAt(dir + "/new/d.sock", true));
        std::string error;
        EXPECT(d.start(error));
        struct stat st{};
        EXPECT_EQ(::stat((dir + "/new").c_str(), &st), 0);
        EXPECT_EQ(int(st.st_mode & 0777), 0700);
    }
    std::filesystem::create_directory(dir + "/open");
    ::chmod((dir + "/open").c_str(), 0755);
    {
        mtd::Daemon d(configAt(dir + "/open/d.sock", true));
        std::string error;
        EXPECT_FALSE(d.start(error));
    }
    // A symlink to a fine directory of ours is still refused: whoever planted it chose the target.
    std::filesystem::create_directory(dir + "/real");
    ::chmod((dir + "/real").c_str(), 0700);
    std::filesystem::create_directory_symlink(dir + "/real", dir + "/link");
    {
        mtd::Daemon d(configAt(dir + "/link/d.sock", true));
        std::string error;
        EXPECT_FALSE(d.start(error));
        EXPECT(!std::filesystem::exists(dir + "/real/d.sock"));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("The umask is restored when binding fails") {
    const auto dir = tempDir();
    const auto before = ::umask(022);
    {
        mtd::Daemon d(configAt(dir + "/missing-dir/d.sock", false)); // Lock file cannot be created.
        std::string error;
        EXPECT_FALSE(d.start(error));
    }
    {
        // Binding fails after the umask was changed: a directory sits where the socket goes.
        std::filesystem::create_directory(dir + "/d.sock");
        mtd::Daemon d(configAt(dir + "/d.sock", false));
        std::string error;
        EXPECT_FALSE(d.start(error));
    }
    const auto now = ::umask(before);
    EXPECT_EQ(int(now), 022);
    std::filesystem::remove_all(dir);
}

// ---- Exposed connections ------------------------------------------------------------------

TEST_CASE("Private, unknown, malformed and ended sessions get byte-identical answers") {
    Running r;
    EXPECT(r.started);
    auto owner = controlClient(r.path);
    const auto priv = createSession(*owner, "");
    const auto ended = createSession(*owner, "");
    EXPECT(priv.has_value() && ended.has_value());
    EXPECT(owner->sendControl(mtd::SetShared{*ended, true}));
    EXPECT(owner->controlReply().has_value());
    DaemonClient mine(r.path);
    EXPECT(attach(mine, *ended, 0).has_value());
    EXPECT(mine.send(protocol::Input{"exit\n"}));
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i) {
        std::this_thread::sleep_for(50ms);
        EXPECT(owner->sendControl(mtd::ListSessions{}));
        gone = std::get<mtd::SessionList>(*owner->controlReply()).sessions.size() == 1;
    }
    EXPECT(gone);
    const auto a = exposedAnswer(r.path, *priv);
    const auto b = exposedAnswer(r.path, "doesnotexist0000");
    const auto c = exposedAnswer(r.path, "bad id with spaces and / slashes");
    const auto d = exposedAnswer(r.path, *ended);
    EXPECT(a.has_value());
    EXPECT(a == b);
    EXPECT(a == c);
    EXPECT(a == d);
}

TEST_CASE("An exposed connection that never says Hello is dropped, a control one is not") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) { c.helloTimeoutMs = 300; });
    EXPECT(r.started);
    auto web = exposedClient(r.path);
    EXPECT(web != nullptr);
    EXPECT(web->atEof(3000));
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    std::this_thread::sleep_for(800ms);
    EXPECT(ctl->sendControl(mtd::GetStatus{}));
    EXPECT(ctl->controlReply().has_value());
}

TEST_CASE("After a confirmed Shutdown nothing new starts, even in the same batch") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    std::string batch, a, b;
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::ControlHello{}), a));
    EXPECT(mtd::encodeFrame(a, batch));
    a.clear();
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::Shutdown{0}), a));
    EXPECT(mtd::encodeFrame(a, batch));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::CreateSession{"", 80, 24}), b));
    EXPECT(mtd::encodeFrame(b, batch));
    EXPECT(c.sendRaw(batch)); // One write: the daemon handles all three before it flushes anything.
    EXPECT(std::holds_alternative<mtd::ControlWelcome>(*c.controlReply()));
    const auto done = c.controlReply();
    EXPECT(done.has_value() && std::holds_alternative<mtd::ShutdownReply>(*done));
    EXPECT(std::get<mtd::ShutdownReply>(*done).done);
    const auto create = c.controlReply();
    EXPECT(create.has_value());
    EXPECT(!std::holds_alternative<mtd::SessionCreated>(*create));
    r.join();
}

TEST_CASE("Limits: sessions and clients") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.registry.maxSessions = 3;
        c.registry.maxDetachedSessions = 3;
        c.maxClients = 4;
    });
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    for (int i = 0; i < 3; ++i) EXPECT(createSession(*ctl, "").has_value());
    EXPECT(ctl->sendControl(mtd::CreateSession{"", 80, 24}));
    const auto full = ctl->controlReply();
    EXPECT(full.has_value() && std::holds_alternative<mtd::ControlError>(*full));
    EXPECT(std::get<mtd::ControlError>(*full).code == mtd::ControlErrorCode::StartFailed);
    // Clients: the control one plus three more fill the limit, the next is closed without a word.
    std::vector<std::unique_ptr<DaemonClient>> held;
    for (int i = 0; i < 3; ++i) held.push_back(controlClient(r.path));
    for (const auto &h : held) EXPECT(h != nullptr);
    DaemonClient extra(r.path);
    EXPECT(extra.connected());
    extra.sendControl(mtd::ControlHello{});
    EXPECT(!extra.frame(500).has_value());
    EXPECT(extra.atEof());
}

// ---- The server's bridge --------------------------------------------------------------------

TEST_CASE("local:<id> parsing: anything odd is refused alike") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    std::optional<std::string> reason;
    for (const std::string &odd : std::vector<std::string>{"local:", "local:../x", "local:a b", "local:" + std::string(58, 'a'),
                                  "local:local:" + *id, "LOCAL:" + *id, "local: " + *id, "local:" + *id + "/",
                                  std::string("local:") + std::string("a\0b", 3)}) {
        Client browser;
        browser.connect(*w.web, cookie);
        protocol::Hello h;
        h.sessionId = odd;
        h.features = protocol::featureReplay;
        browser.send(h);
        const auto m = next(browser);
        EXPECT(m.has_value());
        if (!m) continue;
        EXPECT(std::holds_alternative<protocol::Error>(*m));
        if (const auto *e = std::get_if<protocol::Error>(&*m)) {
            if (!reason) reason = e->reason;
            EXPECT_EQ(e->reason, *reason);
        }
        EXPECT(closes(browser));
    }
    // And none of that attached anything.
    EXPECT(owner->sendControl(mtd::GetStatus{}));
    EXPECT_EQ(std::get<mtd::Status>(*owner->controlReply()).attached, 0u);
}

TEST_CASE("Inside a bridge, a second Hello cannot reach another session") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto pub = createSession(*owner, "");
    const auto priv = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*pub, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*pub));
    EXPECT(std::holds_alternative<protocol::Welcome>(*next(browser)));
    protocol::Hello sneaky;
    sneaky.sessionId = *priv;
    sneaky.features = protocol::featureReplay;
    browser.send(sneaky);
    EXPECT(closes(browser));
    // Control-type frames from the browser are just junk to the session, too.
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    for (const auto &s : std::get<mtd::SessionList>(*owner->controlReply()).sessions) {
        if (s.id == *priv) EXPECT_FALSE(s.attached);
    }
}

TEST_CASE("Logging out cuts the bridge; the session lives on") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*id));
    EXPECT(std::holds_alternative<protocol::Welcome>(*next(browser)));
    EXPECT_EQ(int(call(*w.web, http::verb::post, "/logout", cookie, "http://localhost").result_int()), 200);
    EXPECT(closes(browser));
    std::this_thread::sleep_for(200ms);
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    const auto l = std::get<mtd::SessionList>(*owner->controlReply());
    EXPECT_EQ(l.sessions.size(), std::size_t(1));
    EXPECT_FALSE(l.sessions[0].attached);
    // Logged out: the list is closed too.
    EXPECT_EQ(int(call(*w.web, http::verb::post, "/local-sessions", cookie, "http://localhost").result_int()), 401);
}

TEST_CASE("/local-sessions: cookie, Origin and method are all checked") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    const auto ok = call(*w.web, http::verb::post, "/local-sessions", cookie, "http://localhost");
    EXPECT_EQ(int(ok.result_int()), 200);
    EXPECT(ok.body().find(*id) != std::string::npos);
    for (const auto &bad : {call(*w.web, http::verb::post, "/local-sessions", "", "http://localhost"),
                            call(*w.web, http::verb::post, "/local-sessions", "matterm_session=wrong", "http://localhost"),
                            call(*w.web, http::verb::post, "/local-sessions", cookie, "http://evil.example"),
                            call(*w.web, http::verb::post, "/local-sessions", cookie, ""),
                            call(*w.web, http::verb::get, "/local-sessions", cookie, "http://localhost"),
                            call(*w.web, http::verb::put, "/local-sessions", cookie, "http://localhost"),
                            call(*w.web, http::verb::post, "/local-sessions/", cookie, "http://localhost")}) {
        EXPECT(int(bad.result_int()) != 200);
        EXPECT(bad.body().find(*id) == std::string::npos);
    }
}

TEST_CASE("The daemon going away ends the bridge, and a new daemon does not know the old ids") {
    const auto dir = tempDir();
    std::string id, cookie;
    std::unique_ptr<Fixture> web;
    {
        auto daemon = std::make_unique<Running>(60, std::nullopt, dir);
        daemon->ownsDir = false;
        EXPECT(daemon->started);
        auto owner = controlClient(daemon->path);
        id = *createSession(*owner, "");
        EXPECT(owner->sendControl(mtd::SetShared{id, true}));
        EXPECT(owner->controlReply().has_value());
        web = std::make_unique<Fixture>(std::vector<std::string>{"/bin/sh"}, [&](server::ServerConfig &c) {
            c.exposeLocalSessions = true;
            c.daemonSocket = dir + "/d.sock";
        });
        cookie = web->login();
        Client browser;
        browser.connect(*web, cookie);
        browser.send(localHello(id));
        EXPECT(std::holds_alternative<protocol::Welcome>(*next(browser)));
        daemon->stop(); // The daemon ends: all sessions end with it.
        EXPECT(closes(browser));
    }
    Running again(60, std::nullopt, dir);
    again.ownsDir = false;
    EXPECT(again.started);
    Client browser;
    browser.connect(*web, cookie);
    browser.send(localHello(id));
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
    again.stop();
    web.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("A daemon that never answers does not hold the connection forever") {
    const auto dir = tempDir();
    // A socket that accepts and stays silent.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, (dir + "/silent.sock").c_str(), sizeof addr.sun_path - 1);
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr), 0);
    EXPECT_EQ(::listen(fd, 8), 0);
    Fixture web({"/bin/sh"}, [&](server::ServerConfig &c) {
        c.exposeLocalSessions = true;
        c.daemonSocket = dir + "/silent.sock";
        c.session.helloTimeout = 400ms;
    });
    const auto cookie = web.login();
    Client browser;
    browser.connect(web, cookie);
    browser.send(localHello("abc"));
    const auto t0 = std::chrono::steady_clock::now();
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
    EXPECT(std::chrono::steady_clock::now() - t0 < 5s);
    ::close(fd);
    std::filesystem::remove_all(dir);
}

TEST_CASE("The server only talks to a daemon that belongs to its own user") {
    WithDaemon w(true, {}, [](server::ServerConfig &c) { c.daemonPeerUid = geteuid() + 1; });
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    EXPECT_EQ(int(post(*w.web, "/local-sessions", cookie).result_int()), 502);
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*id));
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
    EXPECT(owner->sendControl(mtd::GetStatus{}));
    EXPECT_EQ(std::get<mtd::Status>(*owner->controlReply()).attached, 0u); // Nothing was sent to it.
}

TEST_CASE("The log names who took what, never what was on the screen") {
    std::ostringstream log;
    auto *saved = std::clog.rdbuf(log.rdbuf());
    std::string id;
    {
        WithDaemon w;
        auto owner = controlClient(w.daemon.path);
        id = *createSession(*owner, "");
        EXPECT(owner->sendControl(mtd::SetShared{id, true}));
        EXPECT(owner->controlReply().has_value());
        const auto cookie = w.web->login();
        Client browser;
        browser.connect(*w.web, cookie);
        browser.send(localHello(id));
        EXPECT(std::holds_alternative<protocol::Welcome>(*next(browser)));
        browser.send(protocol::Input{"echo SCREENTEXT-$((6*7))\n"});
        EXPECT(outputHas(browser, "SCREENTEXT-42"));
        EXPECT(call(*w.web, http::verb::post, "/local-sessions", cookie, "http://localhost").result_int() == 200);
    }
    std::clog.rdbuf(saved);
    const auto text = log.str();
    EXPECT(text.find("SCREENTEXT") == std::string::npos);
    EXPECT(text.find("take-over attempt session=" + id) != std::string::npos);
    EXPECT(text.find("password") == std::string::npos);
    EXPECT(text.find(password) == std::string::npos);
}

TEST_CASE("The e2e helper starts a daemon, shares what it is told to and refuses foreign places") {
    const auto dir = tempDir();
    const auto run = [&](const std::string &args, const std::string &stdinText) {
        const std::string command = "printf '" + stdinText + "' | " + MATTERM_E2E_HELPER + " " + args + " 2>&1";
        std::string out;
        FILE *p = ::popen(command.c_str(), "r");
        char buf[512];
        while (p && std::fgets(buf, sizeof buf, p)) out += buf;
        if (p) ::pclose(p);
        return out;
    };
    const auto out = run("--socket " + dir + "/e.sock --sessions 2 --share 1 --send 'echo E2E-MARK'", "list\\nquit\\n");
    EXPECT(out.find("\"sessions\":[") != std::string::npos);
    std::size_t shared = 0, at = 0;
    while ((at = out.find("\"shared\":true", at)) != std::string::npos) {
        ++shared;
        ++at;
    }
    EXPECT_EQ(shared, std::size_t(2)); // The startup line and the list answer both show the one shared.
    EXPECT(out.find("\"shared\":false") != std::string::npos);
    EXPECT(!std::filesystem::exists(dir + "/e.sock")); // Gone with the helper.
    const auto bad = run("--socket /home/someone/d.sock", "quit\\n");
    EXPECT(bad.find("must be an absolute path under the temp directory") != std::string::npos);
    const auto rel = run("--socket relative.sock", "quit\\n");
    EXPECT(rel.find("must be an absolute path") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_SUIT_END
