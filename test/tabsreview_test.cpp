// Review of the tabs and panes against the server (docs/TABS_REVIEW.md): what a page
// with many panes does to the per-login limits, and what closing a pane leaves
// behind. Real server over loopback; the pages are played by raw clients.
#include "config.h"
#include "mls-unit-test/unittest.h"
#include "serverfixture.h"

#include <memory>
#include <vector>

namespace {
// The defaults of the page: persistent sessions on (needed for a restorable layout),
// 16 upgrades per login, 16 sessions.
void sixteen(server::ServerConfig &config) {
    server::Settings settings;
    settings.server = config;
    server::applyConfigText(
        R"({"persistent_sessions":true,"replay_buffer_bytes":65536,"replay_total_bytes":4194304,"max_upgrades_per_login":16,"max_sessions":16})",
        settings);
    config = settings.server;
    config.process.terminationGrace = std::chrono::milliseconds(0);
}

std::string startSession(Client &client) {
    client.send(protocol::Hello{1, 80, 24, 0, "", protocol::featureReplay});
    const auto message = client.read();
    if (const auto *welcome = std::get_if<protocol::Welcome>(&message)) {
        return welcome->sessionId;
    }
    return {};
}

// A new terminal request answered by an error: its reason, else empty.
std::string refusal(Client &client) {
    client.send(protocol::Hello{1, 80, 24, 0, "", protocol::featureReplay});
    try {
        const auto message = client.read();
        if (const auto *error = std::get_if<protocol::Error>(&message)) {
            return error->reason;
        }
    }
    catch (const std::exception &) {
        return "(socket closed)";
    }
    return {};
}
} // namespace

TEST_SUIT_BEGIN(TabsReview)

TEST_CASE("Closing a pane by dropping its socket leaves the terminal running, "
          "and sixteen of them stop every new pane (what the web page does today)") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"}, sixteen);
    const auto cookie = fixture.login();
    for (int i = 0; i < 16; ++i) {
        Client pane;
        pane.connect(fixture, cookie);
        EXPECT(!startSession(pane).empty());
        // The pane is closed: the page drops the WebSocket and sends nothing else.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Client next;
    next.connect(fixture, cookie);
    EXPECT_EQ(refusal(next), std::string("session limit"));
}

TEST_CASE("The same panes closed with a Close message free their terminals") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"}, sixteen);
    const auto cookie = fixture.login();
    for (int i = 0; i < 16; ++i) {
        Client pane;
        pane.connect(fixture, cookie);
        EXPECT(!startSession(pane).empty());
        pane.send(protocol::Close{});
        // Wait for the end of the terminal before the next one starts.
        for (int k = 0; k < 20; ++k) {
            try {
                const auto message = pane.read();
                if (std::holds_alternative<protocol::Exit>(message)) {
                    break;
                }
            }
            catch (const std::exception &) {
                break;
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Client next;
    next.connect(fixture, cookie);
    EXPECT(!startSession(next).empty());
}

TEST_CASE("A reload that attaches sixteen panes while the old sockets are still open "
          "gets all of them (take-over), and the seventeenth is refused plainly") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"}, sixteen);
    const auto cookie = fixture.login();
    std::vector<std::unique_ptr<Client>> before;
    std::vector<std::string> ids;
    for (int i = 0; i < 16; ++i) {
        before.push_back(std::make_unique<Client>());
        before.back()->connect(fixture, cookie);
        ids.push_back(startSession(*before.back()));
        EXPECT(!ids.back().empty());
    }
    // The page restores its layout from the ids while the old connections live on.
    std::vector<std::unique_ptr<Client>> after;
    int welcomed = 0;
    for (const auto &id : ids) {
        after.push_back(std::make_unique<Client>());
        try {
            after.back()->connect(fixture, cookie);
            after.back()->send(protocol::Hello{1, 80, 24, 0, id, protocol::featureReplay});
            if (std::holds_alternative<protocol::Welcome>(after.back()->read())) {
                ++welcomed;
            }
        }
        catch (const std::exception &) {
        }
    }
    EXPECT_EQ(welcomed, 16);
    // A seventeenth connection: the server is at its connection limit (32) or the
    // login is at its lease limit; either way the page gets a clear end, not a hang.
    Client extra;
    std::string how;
    try {
        extra.connect(fixture, cookie);
        how = refusal(extra);
    }
    catch (const std::exception &) {
        how = "(refused)";
    }
    EXPECT(!how.empty());
}

TEST_CASE("At the lease limit a new pane is refused with a reason the page can show") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec /bin/sh"},
        [](server::ServerConfig &config) {
            sixteen(config);
            server::Settings settings;
            settings.server = config;
            server::applyConfigText(R"({"max_upgrades_per_login":3,"max_sessions":16})", settings);
            config = settings.server;
        });
    const auto cookie = fixture.login();
    std::vector<std::unique_ptr<Client>> panes;
    for (int i = 0; i < 3; ++i) {
        panes.push_back(std::make_unique<Client>());
        panes.back()->connect(fixture, cookie);
        EXPECT(!startSession(*panes.back()).empty());
    }
    Client fourth;
    fourth.connect(fixture, cookie);
    EXPECT_EQ(refusal(fourth), std::string("upgrade limit"));
    // One pane closes: the next attempt works.
    panes.pop_back();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Client again;
    again.connect(fixture, cookie);
    EXPECT(!startSession(again).empty());
}

TEST_SUIT_END
