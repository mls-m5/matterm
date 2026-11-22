#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"

#include <random>

using namespace std::chrono_literals;

TEST_SUIT_BEGIN(Daemon)

TEST_CASE("Framing round trips and survives any chunking") {
    std::string stream;
    EXPECT(mtd::encodeFrame("a", stream));
    EXPECT(mtd::encodeFrame(std::string(mtd::kMaxFrame, 'z'), stream));
    EXPECT(mtd::encodeFrame("hello", stream));
    for (const std::size_t step : {std::size_t(1), std::size_t(3), std::size_t(5), std::size_t(4096), stream.size()}) {
        mtd::FrameDecoder d;
        std::vector<std::string> frames;
        bool ok = true;
        for (std::size_t i = 0; i < stream.size(); i += step) {
            ok = d.feed(stream.substr(i, step), [&](std::string_view f) { frames.emplace_back(f); }) && ok;
        }
        EXPECT(ok);
        EXPECT_EQ(frames.size(), std::size_t(3));
        EXPECT_EQ(frames[0], std::string("a"));
        EXPECT_EQ(frames[1].size(), mtd::kMaxFrame);
        EXPECT_EQ(frames[2], std::string("hello"));
        EXPECT_EQ(d.pending(), std::size_t(0));
    }
}

TEST_CASE("Framing refuses empty, oversized and zero-length frames") {
    std::string out;
    EXPECT_FALSE(mtd::encodeFrame("", out));
    EXPECT_FALSE(mtd::encodeFrame(std::string(mtd::kMaxFrame + 1, 'x'), out));
    EXPECT(out.empty());
    for (const std::string &bad : {std::string("\0\0\0\0", 4), std::string("\0\0\x40\x01", 4), std::string("\xff\xff\xff\xff", 4)}) {
        mtd::FrameDecoder d;
        int frames = 0;
        EXPECT_FALSE(d.feed(bad, [&](std::string_view) { ++frames; }));
        EXPECT_EQ(frames, 0);
        EXPECT(d.failed());
        EXPECT_FALSE(d.feed("\0\0\0\1x", [&](std::string_view) { ++frames; })); // stays failed
        EXPECT_EQ(frames, 0);
    }
    // A partial header is simply waited for.
    mtd::FrameDecoder d;
    EXPECT(d.feed(std::string("\0\0", 2), [](std::string_view) {}));
    EXPECT_EQ(d.pending(), std::size_t(2));
}

TEST_CASE("Framing fuzz: random bytes never crash or exceed the limits") {
    std::mt19937 rng(12345);
    for (int round = 0; round < 2000; ++round) {
        mtd::FrameDecoder d;
        std::size_t total = 0;
        bool ok = true;
        for (int piece = 0; piece < 8 && ok; ++piece) {
            std::string chunk(rng() % 64, '\0');
            for (auto &c : chunk) c = char(rng() % 5 == 0 ? rng() : (rng() % 4 == 0 ? 0 : 1));
            ok = d.feed(chunk, [&](std::string_view f) {
                EXPECT(!f.empty());
                EXPECT(f.size() <= mtd::kMaxFrame);
                total += f.size();
            });
        }
        EXPECT_EQ(ok, !d.failed());
    }
}

TEST_CASE("Control messages round trip and reject malformed input") {
    const std::vector<mtd::ControlRequest> requests{mtd::ControlHello{}, mtd::ListSessions{},
                                                       mtd::SetShared{"abc", true}, mtd::GetStatus{},
                                                       mtd::Shutdown{7}};
    for (const auto &r : requests) {
        std::string b;
        EXPECT(mtd::encode(r, b));
        const auto back = mtd::decodeRequest(b);
        EXPECT(back.has_value());
        EXPECT_EQ(back->index(), r.index());
        EXPECT_FALSE(mtd::decodeRequest(b + "x").has_value());
        EXPECT_FALSE(mtd::decodeRequest(b.substr(0, b.size() - 1)).has_value() && b.size() > 1);
        EXPECT_FALSE(mtd::decodeReply(b).has_value()); // wrong direction
    }
    mtd::SessionList list;
    list.sessions = {{"s1", true, false, 99}, {"s2", false, true, 1u << 20}};
    std::string b;
    EXPECT(mtd::encode(mtd::ControlReply(list), b));
    const auto back = mtd::decodeReply(b);
    EXPECT(back.has_value());
    const auto &l = std::get<mtd::SessionList>(*back);
    EXPECT_EQ(l.sessions.size(), std::size_t(2));
    EXPECT_EQ(l.sessions[1].outputEnd, std::uint64_t(1u << 20));
    EXPECT(l.sessions[0].attached && !l.sessions[0].shared && l.sessions[1].shared);
    EXPECT_FALSE(mtd::decodeRequest("").has_value());
    EXPECT_FALSE(mtd::decodeRequest(std::string("\x42\0\0\0", 4)).has_value()); // empty id
    EXPECT_FALSE(mtd::decodeRequest(std::string("\x40\0\0", 3)).has_value());   // version 0
    EXPECT_FALSE(mtd::decodeReply(std::string("\xc4\0\x09\0\0", 5)).has_value()); // bad code
    std::string tooMany;
    mtd::SessionList big;
    big.sessions.resize(mtd::kMaxListed + 1, {"x", false, false, 0});
    EXPECT_FALSE(mtd::encode(mtd::ControlReply(big), tooMany));
    // The terminal protocol does not understand control types, and the reverse.
    EXPECT_FALSE(protocol::decodeClient(std::string("\x41", 1)));
    std::mt19937 rng(7);
    for (int i = 0; i < 3000; ++i) {
        std::string junk(rng() % 24, '\0');
        for (auto &c : junk) c = char(rng());
        if (!junk.empty() && i % 2) junk[0] = char(0x40 + rng() % 5);
        if (!junk.empty() && i % 3 == 0) junk[0] = char(0xc0 + rng() % 5);
        (void)mtd::decodeRequest(junk);
        (void)mtd::decodeReply(junk);
    }
}

TEST_CASE("A session runs through the socket and has the daemon feature") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(c.connected());
    const auto w = attach(c);
    EXPECT(w.has_value());
    EXPECT((w->features & protocol::featureDaemon) != 0);
    EXPECT((w->features & protocol::featureReplay) != 0);
    EXPECT(c.send(protocol::Input{"echo daemon-$((20+22))\n"}));
    EXPECT(c.outputContains("daemon-42"));
}

TEST_CASE("The socket is 0600 and the lock file names the pid") {
    Running r;
    EXPECT(r.started);
    struct stat st{};
    EXPECT_EQ(::stat(r.path.c_str(), &st), 0);
    EXPECT_EQ(int(st.st_mode & 0777), 0600);
    EXPECT(std::filesystem::exists(r.path + ".lock"));
}

TEST_CASE("Sessions survive a client disconnect and replay after reconnect") {
    Running r;
    EXPECT(r.started);
    std::string id;
    {
        DaemonClient c(r.path);
        const auto w = attach(c);
        EXPECT(w.has_value());
        id = w->sessionId;
        EXPECT(c.send(protocol::Input{"echo before-$((1+1))\n"}));
        EXPECT(c.outputContains("before-2"));
    }
    std::this_thread::sleep_for(200ms);
    {
        DaemonClient ctl(r.path);
        EXPECT(ctl.sendControl(mtd::ControlHello{}));
        const auto welcome = ctl.controlReply();
        EXPECT(welcome.has_value() && std::holds_alternative<mtd::ControlWelcome>(*welcome));
        EXPECT(ctl.sendControl(mtd::ListSessions{}));
        const auto list = ctl.controlReply();
        EXPECT(list.has_value());
        const auto &l = std::get<mtd::SessionList>(*list);
        EXPECT_EQ(l.sessions.size(), std::size_t(1));
        EXPECT_EQ(l.sessions[0].id, id);
        EXPECT_FALSE(l.sessions[0].attached);
        EXPECT_FALSE(l.sessions[0].shared);
    }
    DaemonClient again(r.path);
    const auto w = attach(again, id, 0);
    EXPECT(w.has_value());
    EXPECT_EQ(w->sessionId, id);
    EXPECT(again.outputContains("before-2"));
    EXPECT(again.send(protocol::Input{"echo after-$((2+2))\n"}));
    EXPECT(again.outputContains("after-4"));
}

TEST_CASE("Marking a session shared is visible in the list and refused for unknown ids") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    const auto w = attach(c);
    EXPECT(w.has_value());
    DaemonClient ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
    EXPECT(ctl.sendControl(mtd::SetShared{w->sessionId, true}));
    auto reply = ctl.controlReply();
    EXPECT(reply.has_value());
    EXPECT(std::get<mtd::SessionList>(*reply).sessions.at(0).shared);
    EXPECT(ctl.sendControl(mtd::SetShared{"nonexistent", true}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value() && std::holds_alternative<mtd::ControlError>(*reply));
    EXPECT(ctl.sendControl(mtd::GetStatus{}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value());
    const auto &s = std::get<mtd::Status>(*reply);
    EXPECT_EQ(s.sessions, 1u);
    EXPECT_EQ(s.attached, 1u);
    EXPECT_EQ(s.shared, 1u);
    EXPECT_EQ(s.clients, 2u);
}

TEST_CASE("Another user is refused without a word") {
    Running r(60, geteuid() + 1);
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(c.connected());
    c.send(hello());
    EXPECT(!c.frame(1000).has_value());
    EXPECT(c.atEof());
}

TEST_CASE("Framing violations on the socket close the connection") {
    Running r;
    EXPECT(r.started);
    for (const std::string &bad : {std::string("\0\0\0\0", 4), std::string("\0\0\x40\x01xx", 6),
                                  std::string("\xde\xad\xbe\xef", 4)}) {
        DaemonClient c(r.path);
        EXPECT(c.sendRaw(bad));
        EXPECT(c.atEof());
    }
    // And the daemon is still fine.
    DaemonClient ok(r.path);
    EXPECT(attach(ok).has_value());
}

TEST_CASE("A first frame that is neither Hello nor ControlHello is an error") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(c.send(protocol::Input{"x"}));
    const auto m = c.server();
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(c.atEof());
    DaemonClient silent(r.path); // Says nothing: dropped after the hello timeout (default 5 s) - not waited for here.
    EXPECT(silent.connected());
}

TEST_CASE("Idle exit: no sessions and no client") {
    Running r(1);
    EXPECT(r.started);
    const auto t0 = std::chrono::steady_clock::now();
    r.join();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT(r.d->reason() == mtd::ExitReason::Idle);
    EXPECT(took < 20s);
    EXPECT(!std::filesystem::exists(r.path));
}

TEST_CASE("A session keeps the daemon alive past the idle time") {
    Running r(1);
    EXPECT(r.started);
    {
        DaemonClient c(r.path);
        EXPECT(attach(c).has_value());
    }
    std::this_thread::sleep_for(2500ms);
    EXPECT(r.d->reason() == mtd::ExitReason::None);
    DaemonClient ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
}

TEST_CASE("Two daemons cannot share a socket") {
    Running first;
    EXPECT(first.started);
    Running second(60, std::nullopt, first.dir);
    second.ownsDir = false;
    EXPECT_FALSE(second.started);
    EXPECT(second.startError.find("already") != std::string::npos);
    // The first one is untouched.
    DaemonClient c(first.path);
    EXPECT(attach(c).has_value());
}

TEST_CASE("A stale socket file from a dead daemon is replaced") {
    std::string t = (std::filesystem::temp_directory_path() / "mtdXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    {
        Running a(60, std::nullopt, dir);
        a.ownsDir = false;
        EXPECT(a.started);
    }
    // Make the leftovers look like a crash: a socket file with nobody behind it.
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, (dir + "/d.sock").c_str(), sizeof addr.sun_path - 1);
        ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        ::close(fd);
    }
    Running b(60, std::nullopt, dir);
    b.ownsDir = false;
    EXPECT(b.started);
    DaemonClient c(b.path);
    EXPECT(attach(c).has_value());
    b.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Shutdown runs only with the confirmed session count") {
    Running r;
    EXPECT(r.started);
    DaemonClient c(r.path);
    EXPECT(attach(c).has_value());
    DaemonClient ctl(r.path);
    EXPECT(ctl.sendControl(mtd::ControlHello{}));
    EXPECT(ctl.controlReply().has_value());
    EXPECT(ctl.sendControl(mtd::Shutdown{0})); // The client believed there were none.
    auto reply = ctl.controlReply();
    EXPECT(reply.has_value());
    const auto &refused = std::get<mtd::ShutdownReply>(*reply);
    EXPECT_FALSE(refused.done);
    EXPECT_EQ(refused.sessions, 1u);
    std::this_thread::sleep_for(100ms);
    EXPECT(r.d->reason() == mtd::ExitReason::None);
    EXPECT(ctl.sendControl(mtd::Shutdown{1}));
    reply = ctl.controlReply();
    EXPECT(reply.has_value());
    EXPECT(std::get<mtd::ShutdownReply>(*reply).done);
    r.join();
    EXPECT(r.d->reason() == mtd::ExitReason::Shutdown);
    EXPECT(!std::filesystem::exists(r.path));
    EXPECT(c.atEof());
}

TEST_CASE("CreateSession starts a shell in the folder and GetSessionInfo reads it back") {
    Running r;
    EXPECT(r.started);
    std::string t = (std::filesystem::temp_directory_path() / "mtcwdXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    const auto id = createSession(*ctl, dir);
    EXPECT(id.has_value());
    DaemonClient c(r.path);
    const auto w = attach(c, *id, 0);
    EXPECT(w.has_value());
    EXPECT_EQ(w->sessionId, *id);
    EXPECT(c.send(protocol::Input{"pwd\n"}));
    EXPECT(c.outputContains(dir));
    auto i = info(*ctl, *id);
    EXPECT(i.has_value());
    EXPECT_EQ(i->cwd, dir);
    EXPECT_FALSE(i->busy);
    EXPECT(c.send(protocol::Input{"exec sleep 30\n"}));
    bool busy = false;
    for (int n = 0; n < 50 && !busy; ++n) {
        std::this_thread::sleep_for(100ms);
        i = info(*ctl, *id);
        busy = i && i->busy;
    }
    EXPECT(busy);
    EXPECT_EQ(i->command.size(), std::size_t(2));
    EXPECT_EQ(i->command.at(0), std::string("sleep"));
    EXPECT(!info(*ctl, "nonexistent").has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("CreateSession ignores a folder that does not exist and refuses nothing else it was not asked") {
    Running r;
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    EXPECT(createSession(*ctl, "/nonexistent/for/sure").has_value());
    EXPECT(createSession(*ctl, "relative/path").has_value());
    EXPECT(createSession(*ctl, "").has_value());
    EXPECT(ctl->sendControl(mtd::ListSessions{}));
    const auto l = ctl->controlReply();
    EXPECT_EQ(std::get<mtd::SessionList>(*l).sessions.size(), std::size_t(3));
}

TEST_CASE("Control codec: the session messages round trip") {
    std::string b;
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::CreateSession{"/tmp", 80, 24}), b));
    auto q = mtd::decodeRequest(b);
    EXPECT(q.has_value());
    EXPECT_EQ(std::get<mtd::CreateSession>(*q).cwd, std::string("/tmp"));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::CreateSession{"/tmp", 0, 24}), b));
    EXPECT_FALSE(mtd::decodeRequest(b).has_value());
    b.clear();
    mtd::SessionInfo si{"abc", "/home/x", true, {"vim", "a b"}};
    EXPECT(mtd::encode(mtd::ControlReply(si), b));
    auto p = mtd::decodeReply(b);
    EXPECT(p.has_value());
    const auto &got = std::get<mtd::SessionInfo>(*p);
    EXPECT(got.busy);
    EXPECT_EQ(got.command.size(), std::size_t(2));
    EXPECT_EQ(got.command[1], std::string("a b"));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::ExposedHello{}), b));
    EXPECT(std::holds_alternative<mtd::ExposedHello>(*mtd::decodeRequest(b)));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlReply(mtd::SessionCreated{"id1"}), b));
    EXPECT_EQ(std::get<mtd::SessionCreated>(*mtd::decodeReply(b)).id, std::string("id1"));
}

TEST_CASE("Names: SetName round trips, bad names are refused, old peers get the old types without names") {
    std::string b;
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::SetName{"abc", "build box \xc3\xa5"}), b));
    const auto q = mtd::decodeRequest(b);
    EXPECT(q.has_value());
    EXPECT_EQ(std::get<mtd::SetName>(*q).name, std::string("build box \xc3\xa5"));
    for (const std::string &bad : {std::string("a\nb"), std::string("a\x1b" "b"), std::string("a\x7f"), std::string("\xc2\x85"),
                                  std::string("\xff"), std::string(65, 'x')}) {
        b.clear();
        EXPECT(!mtd::encode(mtd::ControlRequest(mtd::SetName{"abc", bad}), b)); // never written
    }
    EXPECT(mtd::validTerminalName(""));
    EXPECT(mtd::validTerminalName(std::string(64, 'x')));
    // A named list: the new type for a peer of version 3, the old one for version 2.
    mtd::SessionList list;
    list.sessions.push_back({"s1", true, false, 7, "my name"});
    b.clear();
    EXPECT(mtd::encode(mtd::ControlReply(list), b, 3));
    EXPECT_EQ(int(std::uint8_t(b[0])), 0xc7);
    EXPECT_EQ(std::get<mtd::SessionList>(*mtd::decodeReply(b)).sessions[0].name, std::string("my name"));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlReply(list), b, 2));
    EXPECT_EQ(int(std::uint8_t(b[0])), 0xc1);
    const auto old = std::get<mtd::SessionList>(*mtd::decodeReply(b));
    EXPECT(old.sessions[0].name.empty() && old.sessions[0].attached && old.sessions[0].outputEnd == 7);
    mtd::SessionInfo info{"abc", "/x", false, {}, "named"};
    b.clear();
    EXPECT(mtd::encode(mtd::ControlReply(info), b, 3));
    EXPECT_EQ(std::get<mtd::SessionInfo>(*mtd::decodeReply(b)).name, std::string("named"));
    b.clear();
    EXPECT(mtd::encode(mtd::ControlReply(info), b, 2));
    EXPECT(std::get<mtd::SessionInfo>(*mtd::decodeReply(b)).name.empty());
    // A truncated or trailing-byte named reply is refused.
    b.clear();
    mtd::encode(mtd::ControlReply(list), b, 3);
    EXPECT(!mtd::decodeReply(b.substr(0, b.size() - 1)).has_value());
    EXPECT(!mtd::decodeReply(b + "x").has_value());
}

TEST_CASE("The daemon stores a name, lists it to its owner and to the web server for shared terminals only") {
    Running r;
    EXPECT(r.started);
    auto owner = controlClient(r.path);
    const auto shared = createSession(*owner, "");
    const auto priv = createSession(*owner, "");
    EXPECT(shared.has_value() && priv.has_value());
    EXPECT(owner->sendControl(mtd::SetName{*shared, "laptop build"}));
    auto reply = owner->controlReply();
    EXPECT(reply.has_value() && std::holds_alternative<mtd::SessionList>(*reply));
    EXPECT(owner->sendControl(mtd::SetName{*priv, "secret"}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(owner->sendControl(mtd::SetName{"nonexistent", "x"}));
    reply = owner->controlReply();
    EXPECT(reply.has_value() && std::holds_alternative<mtd::ControlError>(*reply));
    EXPECT(owner->sendControl(mtd::SetShared{*shared, true}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(owner->sendControl(mtd::GetSessionInfo{*shared}));
    reply = owner->controlReply();
    EXPECT(reply.has_value());
    EXPECT_EQ(std::get<mtd::SessionInfo>(*reply).name, std::string("laptop build"));
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    reply = owner->controlReply();
    EXPECT_EQ(std::get<mtd::SessionList>(*reply).sessions.size(), std::size_t(2));
    // The web server sees the shared one with its name, and nothing of the private one.
    auto web = exposedClient(r.path);
    EXPECT(web->sendControl(mtd::ListSessions{}));
    const auto seen = std::get<mtd::SessionList>(*web->controlReply()).sessions;
    EXPECT_EQ(seen.size(), std::size_t(1));
    EXPECT_EQ(seen[0].name, std::string("laptop build"));
    // It cannot name anything.
    auto web2 = exposedClient(r.path);
    EXPECT(web2->sendControl(mtd::SetName{*shared, "hijack"}));
    const auto refused = web2->controlReply();
    EXPECT(refused.has_value() && std::holds_alternative<mtd::ControlError>(*refused));
    // The name can be removed again.
    EXPECT(owner->sendControl(mtd::SetName{*shared, ""}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(owner->sendControl(mtd::GetSessionInfo{*shared}));
    EXPECT(std::get<mtd::SessionInfo>(*owner->controlReply()).name.empty());
}

TEST_CASE("An exposed connection sees and attaches to shared sessions only") {
    Running r;
    EXPECT(r.started);
    auto owner = controlClient(r.path);
    EXPECT(owner != nullptr);
    const auto priv = createSession(*owner, "");
    const auto pub = createSession(*owner, "");
    EXPECT(priv.has_value() && pub.has_value());
    EXPECT(owner->sendControl(mtd::SetShared{*pub, true}));
    EXPECT(owner->controlReply().has_value());
    {
        auto web = exposedClient(r.path);
        EXPECT(web != nullptr);
        EXPECT(web->sendControl(mtd::ListSessions{}));
        const auto l = web->controlReply();
        EXPECT(l.has_value());
        const auto &sessions = std::get<mtd::SessionList>(*l).sessions;
        EXPECT_EQ(sessions.size(), std::size_t(1));
        EXPECT_EQ(sessions[0].id, *pub);
    }
    {
        // The private session cannot be attached, and looks like one that does not exist.
        auto web = exposedClient(r.path);
        EXPECT(web != nullptr);
        EXPECT(web->send(hello(*priv)));
        const auto m = web->server();
        EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
        EXPECT(web->atEof());
        auto web2 = exposedClient(r.path);
        EXPECT(web2->send(hello("nonexistent")));
        const auto m2 = web2->server();
        EXPECT(m2.has_value() && std::holds_alternative<protocol::Error>(*m2));
        EXPECT_EQ(std::get<protocol::Error>(*m2).reason, std::get<protocol::Error>(*m).reason);
    }
    {
        // Nor can it create, probe or end anything.
        for (const mtd::ControlRequest &req : std::vector<mtd::ControlRequest>{
                 mtd::CreateSession{"", 80, 24}, mtd::GetSessionInfo{*priv}, mtd::SetShared{*priv, true},
                 mtd::Shutdown{2}, mtd::GetStatus{}}) {
            auto web = exposedClient(r.path);
            EXPECT(web != nullptr);
            EXPECT(web->sendControl(req));
            const auto e = web->controlReply();
            EXPECT(e.has_value() && std::holds_alternative<mtd::ControlError>(*e));
            EXPECT(web->atEof());
        }
        auto web = exposedClient(r.path);
        EXPECT(web->send(hello())); // No session id: would create one.
        const auto m = web->server();
        EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
        EXPECT(owner->sendControl(mtd::ListSessions{}));
        EXPECT_EQ(std::get<mtd::SessionList>(*owner->controlReply()).sessions.size(), std::size_t(2));
    }
    // The shared one works, and stopping the sharing cuts the viewer but not the session.
    auto web = exposedClient(r.path);
    EXPECT(web != nullptr);
    const auto w = attach(*web, *pub, 0);
    EXPECT(w.has_value());
    EXPECT(web->send(protocol::Input{"echo seen-$((6*7))\n"}));
    EXPECT(web->outputContains("seen-42"));
    EXPECT(owner->sendControl(mtd::SetShared{*pub, false}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(web->atEof());
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    EXPECT_EQ(std::get<mtd::SessionList>(*owner->controlReply()).sessions.size(), std::size_t(2));
    // Taking the session back as its owner works (it is merely detached).
    DaemonClient mine(r.path);
    EXPECT(attach(mine, *pub, 0).has_value());
    EXPECT(mine.outputContains("seen-42"));
}

TEST_SUIT_END
