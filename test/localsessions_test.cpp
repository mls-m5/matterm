// matterm-server --expose-local-sessions against a real daemon, both in temp directories.
#include "config.h"
#include "localfixture.h"
#include "mls-unit-test/unittest.h"

using namespace std::chrono_literals;

TEST_SUIT_BEGIN(LocalSessions)

TEST_CASE("An unshared session is invisible even with the right password") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    EXPECT(owner != nullptr);
    const auto priv = createSession(*owner, "");
    EXPECT(priv.has_value());
    const auto cookie = w.web->login();
    const auto listed = post(*w.web, "/local-sessions", cookie);
    EXPECT_EQ(int(listed.result_int()), 200);
    EXPECT(listed.body().find(*priv) == std::string::npos);
    EXPECT(listed.body().find("\"sessions\":[]") != std::string::npos);
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*priv));
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
    // Same answer for a session that never existed.
    Client other;
    other.connect(*w.web, cookie);
    other.send(localHello("doesnotexist"));
    const auto m2 = next(other);
    EXPECT(m2.has_value() && std::holds_alternative<protocol::Error>(*m2));
    EXPECT_EQ(std::get<protocol::Error>(*m2).reason, std::get<protocol::Error>(*m).reason);
    // Still there for its owner.
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    EXPECT_EQ(std::get<mtd::SessionList>(*owner->controlReply()).sessions.size(), std::size_t(1));
}

TEST_CASE("A shared session is listed, can be driven from the browser and taken back") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(id.has_value());
    // The owner's window is attached first.
    DaemonClient mine(w.daemon.path);
    EXPECT(attach(mine, *id, 0).has_value());
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    EXPECT_EQ(int(post(*w.web, "/local-sessions", "").result_int()), 401);
    const auto listed = post(*w.web, "/local-sessions", cookie);
    EXPECT_EQ(int(listed.result_int()), 200);
    EXPECT(listed.body().find("\"id\":\"" + *id + "\"") != std::string::npos);
    EXPECT(listed.body().find("\"attached\":true") != std::string::npos);

    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*id));
    const auto welcome = next(browser);
    EXPECT(welcome.has_value() && std::holds_alternative<protocol::Welcome>(*welcome));
    EXPECT_EQ(std::get<protocol::Welcome>(*welcome).sessionId, *id);
    // Taking over is exclusive: the laptop window is told.
    const auto lost = mine.server();
    EXPECT(lost.has_value());
    bool replaced = false;
    for (int i = 0; i < 20 && !replaced; ++i) {
        const auto m = i == 0 ? lost : mine.server();
        replaced = m && std::holds_alternative<protocol::Error>(*m) &&
                   std::get<protocol::Error>(*m).code == protocol::ErrorCode::Replaced;
    }
    EXPECT(replaced);
    browser.send(protocol::Input{"echo from-phone-$((40+2))\n"});
    EXPECT(outputHas(browser, "from-phone-42"));
    // The owner takes it back (the daemon sends the replay, the browser is told).
    DaemonClient again(w.daemon.path);
    const auto back = attach(again, *id, 0);
    EXPECT(back.has_value());
    EXPECT(again.outputContains("from-phone-42"));
    bool told = false;
    for (int i = 0; i < 50 && !told; ++i) {
        const auto m = next(browser);
        if (!m) break;
        told = std::holds_alternative<protocol::Error>(*m) &&
               std::get<protocol::Error>(*m).code == protocol::ErrorCode::Replaced;
    }
    EXPECT(told);
    EXPECT(closes(browser));
}

TEST_CASE("Sharing one terminal never exposes a terminal made later, also not after a take-over and take-back") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto a = createSession(*owner, "");
    EXPECT(a.has_value());
    DaemonClient mineA(w.daemon.path);
    EXPECT(attach(mineA, *a, 0).has_value());
    EXPECT(owner->sendControl(mtd::SetShared{*a, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    // The phone takes A over.
    Client phone;
    phone.connect(*w.web, cookie);
    phone.send(localHello(*a));
    const auto welcome = next(phone);
    EXPECT(welcome.has_value() && std::holds_alternative<protocol::Welcome>(*welcome));
    // The owner opens a second tab: a new terminal, attached by the window, not shared.
    const auto b = createSession(*owner, "");
    EXPECT(b.has_value());
    DaemonClient mineB(w.daemon.path);
    EXPECT(attach(mineB, *b, 0).has_value());
    // The owner takes A back; B must not have become visible by any of this.
    DaemonClient backA(w.daemon.path);
    EXPECT(attach(backA, *a, 0).has_value());
    const auto listed = post(*w.web, "/local-sessions", cookie);
    EXPECT_EQ(int(listed.result_int()), 200);
    EXPECT(listed.body().find("\"id\":\"" + *a + "\"") != std::string::npos);
    EXPECT(listed.body().find(*b) == std::string::npos);
    // B cannot be taken over, with the same answer as for a session that does not exist.
    Client attacker;
    attacker.connect(*w.web, cookie);
    attacker.send(localHello(*b));
    const auto refused = next(attacker);
    EXPECT(refused.has_value() && std::holds_alternative<protocol::Error>(*refused));
    EXPECT(closes(attacker));
    // B still belongs to the window (no Replaced reached it).
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    const auto all = std::get<mtd::SessionList>(*owner->controlReply()).sessions;
    bool bAttached = false, bShared = true;
    for (const auto &e : all) {
        if (e.id == *b) {
            bAttached = e.attached;
            bShared = e.shared;
        }
    }
    EXPECT(bAttached && !bShared);
}

TEST_CASE("The list for the browser carries the owner's name for a terminal, escaped") {
    WithDaemon w;
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(id.has_value());
    EXPECT(owner->sendControl(mtd::SetName{*id, "say \"hi\" \\ ok"}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto listed = post(*w.web, "/local-sessions", w.web->login());
    EXPECT_EQ(int(listed.result_int()), 200);
    EXPECT(listed.body().find("\"name\":\"say \\\"hi\\\" \\\\ ok\"") != std::string::npos);
}

TEST_CASE("Stopping the sharing cuts the browser and keeps the session") {
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
    EXPECT(owner->sendControl(mtd::SetShared{*id, false}));
    EXPECT(owner->controlReply().has_value());
    EXPECT(closes(browser));
    EXPECT(owner->sendControl(mtd::ListSessions{}));
    EXPECT_EQ(std::get<mtd::SessionList>(*owner->controlReply()).sessions.size(), std::size_t(1));
    EXPECT(post(*w.web, "/local-sessions", cookie).body().find(*id) == std::string::npos);
}

TEST_CASE("Without --expose-local-sessions the server never offers or bridges anything") {
    WithDaemon w(false);
    auto owner = controlClient(w.daemon.path);
    const auto id = createSession(*owner, "");
    EXPECT(owner->sendControl(mtd::SetShared{*id, true}));
    EXPECT(owner->controlReply().has_value());
    const auto cookie = w.web->login();
    EXPECT_EQ(int(post(*w.web, "/local-sessions", cookie).result_int()), 404);
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello(*id));
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
    EXPECT(owner->sendControl(mtd::GetStatus{}));
    EXPECT_EQ(std::get<mtd::Status>(*owner->controlReply()).attached, 0u); // Nobody got in.
}

TEST_CASE("A missing daemon is reported, not hung on") {
    WithDaemon w(true, "/nonexistent-dir/none.sock");
    const auto cookie = w.web->login();
    EXPECT_EQ(int(post(*w.web, "/local-sessions", cookie).result_int()), 502);
    Client browser;
    browser.connect(*w.web, cookie);
    browser.send(localHello("abc"));
    const auto m = next(browser);
    EXPECT(m.has_value() && std::holds_alternative<protocol::Error>(*m));
    EXPECT(closes(browser));
}

TEST_CASE("Ordinary browser sessions still work next to the exposure") {
    WithDaemon w;
    const auto cookie = w.web->login();
    Client browser;
    browser.connect(*w.web, cookie);
    protocol::Hello h;
    browser.send(h);
    EXPECT(std::holds_alternative<protocol::Welcome>(*next(browser)));
    browser.send(protocol::Input{"echo ordinary-$((1+2))\n"});
    EXPECT(outputHas(browser, "ordinary-3"));
}

TEST_CASE("Command line: remote listening with exposure needs its own option") {
    const auto none = [](const char *) -> const char * { return nullptr; };
    const auto resolve = [&](std::vector<std::string> args) {
        return server::resolveSettings(server::parseCommandLine(args), none);
    };
    EXPECT(resolve({"--expose-local-sessions"}).server.exposeLocalSessions);
    EXPECT(resolve({"--expose-local-sessions", "--daemon-socket", "/tmp/x.sock"}).server.daemonSocket ==
           "/tmp/x.sock");
    bool refused = false;
    try {
        resolve({"--expose-local-sessions", "--listen", "0.0.0.0", "--allow-plain-http"});
    }
    catch (const server::ConfigError &e) {
        refused = std::string(e.what()).find("--allow-remote-listen") != std::string::npos;
    }
    EXPECT(refused);
    EXPECT(resolve({"--expose-local-sessions", "--listen", "0.0.0.0", "--allow-plain-http",
                    "--allow-remote-listen"})
               .server.allowRemoteListen);
    // The option alone without exposure is harmless, a socket without exposure is a mistake.
    EXPECT(!resolve({"--listen", "0.0.0.0", "--allow-plain-http"}).server.exposeLocalSessions);
    bool socketRefused = false;
    try {
        resolve({"--daemon-socket", "/tmp/x.sock"});
    }
    catch (const server::ConfigError &) {
        socketRefused = true;
    }
    EXPECT(socketRefused);
}

TEST_SUIT_END
