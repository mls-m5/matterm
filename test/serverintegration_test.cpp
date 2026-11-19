#include "mls-unit-test/unittest.h"
#include "serverfixture.h"
#include <filesystem>
#include <fstream>

TEST_SUIT_BEGIN(ServerIntegration)
TEST_CASE("Login errors have the same shape and Origin is strict") {
    Fixture fixture;
    auto wrong =
        fixture.request(http::verb::post, "/login", "{\"password\":\"wrong\"}");
    auto malformed = fixture.request(http::verb::post, "/login", "bad json");
    EXPECT(wrong.result() == http::status::unauthorized);
    EXPECT_EQ(wrong.body(), malformed.body());
    EXPECT(wrong.result() == malformed.result());
    EXPECT(
        fixture.request(http::verb::post, "/login", "{}", "http://evil.example")
            .result() == http::status::forbidden);
    EXPECT(fixture.request(http::verb::get, "/").result() == http::status::ok);
    auto login = fixture.request(
        http::verb::post, "/login", "{\"password\":\"" + password + "\"}");
    EXPECT(login.result() == http::status::ok);
    const std::string cookie(login[http::field::set_cookie]);
    EXPECT(cookie.find("HttpOnly; Secure; SameSite=Strict; Path=/") !=
           std::string::npos);
}
TEST_CASE("Upgrade requires cookie, strict Origin and subprotocol") {
    Fixture fixture;
    const auto cookie = fixture.login();
    for (int which = 0; which < 4; ++which) {
        Client client;
        bool refused = false;
        try {
            client.connect(fixture,
                           which == 0   ? ""
                           : which == 1 ? "matterm_session=forged"
                                        : cookie,
                           which == 2 ? "http://evil.example"
                                      : "http://localhost",
                           which == 3 ? "other" : "matterm-tty.v1");
        }
        catch (const boost::system::system_error &) {
            refused = true;
        }
        EXPECT(refused);
    }
}
TEST_CASE("Hello shell output resize and ordered exit, with reaping") {
    Fixture fixture;
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    const auto initial = client.initialOutput();
    const int pid = childPid(initial);
    EXPECT(pid > 0);
    client.send(protocol::Resize{120, 40, 960, 640});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    client.send(protocol::Input{"echo hello; stty size; exit 7\n"});
    std::string output;
    int exitCode = -999;
    for (unsigned i = 0; i < 100; ++i) {
        auto message = client.read();
        if (auto *bytes = std::get_if<protocol::Output>(&message)) {
            output += bytes->data;
            client.send(protocol::Ack{bytes->offset + bytes->data.size()});
        }
        if (auto *exit = std::get_if<protocol::Exit>(&message)) {
            exitCode = exit->code;
            break;
        }
    }
    EXPECT_EQ(exitCode, 7);
    EXPECT(output.find("\r\nhello\r\n") != std::string::npos);
    EXPECT(output.find("40 120") != std::string::npos);
    EXPECT(gone(pid));
    beast::flat_buffer buffer;
    beast::error_code ec;
    client.socket.read(buffer, ec);
    EXPECT(ec == ws::error::closed);
    EXPECT_EQ(client.socket.reason().code, 1000);
}
TEST_CASE("Oversize and text messages close the transport") {
    for (bool text : {false, true}) {
        Fixture fixture;
        Client client;
        client.connect(fixture, fixture.login());
        client.socket.binary(!text);
        const std::string bytes(text ? 1 : 16385, 'x');
        client.socket.write(net::buffer(bytes));
        beast::flat_buffer buffer;
        beast::error_code ec;
        client.socket.read(buffer, ec);
        EXPECT(ec == ws::error::closed);
        EXPECT_EQ(client.socket.reason().code, (text ? 1002 : 1009));
    }
}
TEST_CASE("Disconnect kills and reaps a HUP-ignoring shell") {
    Fixture fixture({"/bin/sh",
                     "-c",
                     "trap '' HUP; printf 'CHILD:%s\\n' \"$$\"; while :; do "
                     "sleep 1; done"});
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    const int pid = childPid(client.initialOutput());
    EXPECT(pid > 0);
    beast::error_code ignored;
    client.socket.next_layer().socket().close(ignored);
    EXPECT(gone(pid));
}
TEST_CASE("A leftover background process dies with the session leader") {
    Fixture fixture({"/bin/sh",
                     "-c",
                     "trap '' HUP; printf 'CHILD:%s\\n' \"$$\"; "
                     "sleep 300 </dev/null >/dev/null 2>&1 & "
                     "printf 'BG:%s\\n' \"$!\"; exit 3"});
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    std::string output;
    int exitCode = -999;
    for (unsigned i = 0; i < 100 && exitCode == -999; ++i) {
        auto message = client.read();
        if (auto *bytes = std::get_if<protocol::Output>(&message)) {
            output += bytes->data;
            client.send(protocol::Ack{bytes->offset + bytes->data.size()});
        }
        if (auto *exit = std::get_if<protocol::Exit>(&message))
            exitCode = exit->code;
    }
    EXPECT_EQ(exitCode, 3);
    const auto at = output.find("BG:");
    EXPECT(at != std::string::npos);
    const int background =
        at == std::string::npos ? -1 : std::stoi(output.substr(at + 3));
    EXPECT(gone(background));
}
TEST_CASE("Close with a full unacknowledged window still ends and cleans up") {
    Fixture fixture(
        {"/bin/sh", "-c", "printf 'CHILD:%s\\n' \"$$\"; exec yes flood"});
    Client client;
    client.connect(fixture, fixture.login());
    client.send(protocol::Hello{1, 80, 24, 0, "", 0});
    EXPECT(std::holds_alternative<protocol::Welcome>(client.read()));
    // Read without acknowledging until the window (256 KiB) is exhausted.
    std::string output;
    std::size_t received = 0;
    while (received < 262144) {
        auto message = client.read();
        if (auto *bytes = std::get_if<protocol::Output>(&message)) {
            output += bytes->data;
            received += bytes->data.size();
        }
    }
    const int pid = childPid(output);
    EXPECT(pid > 0);
    client.send(protocol::Close{});
    bool sawExit = false;
    beast::error_code ec;
    for (unsigned i = 0; i < 200 && !ec; ++i) {
        beast::flat_buffer buffer;
        client.socket.read(buffer, ec);
        if (!ec) {
            auto decoded =
                protocol::decodeServer(beast::buffers_to_string(buffer.data()));
            if (decoded &&
                std::holds_alternative<protocol::Exit>(*decoded.message))
                sawExit = true;
        }
    }
    EXPECT(ec == ws::error::closed);
    EXPECT(!sawExit);
    EXPECT(gone(pid));
}
TEST_CASE("Static client files are served by exact name only") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("matterm-static-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "<title>client</title>";
    std::ofstream(dir / "app.js") << "var x;";
    std::ofstream(dir / ".hidden") << "secret";
    {
        Fixture fixture({"/bin/sh"}, [&](server::ServerConfig &config) {
            config.staticDir = dir.string();
        });
        auto index = fixture.request(http::verb::get, "/");
        EXPECT(index.result() == http::status::ok);
        EXPECT_EQ(index.body(), "<title>client</title>");
        EXPECT(std::string(index["Content-Security-Policy"])
                   .find("script-src 'self'") != std::string::npos);
        auto script = fixture.request(http::verb::get, "/app.js?v=1");
        EXPECT(script.result() == http::status::ok);
        EXPECT_EQ(std::string(script[http::field::content_type]),
                  "text/javascript; charset=utf-8");
        for (const char *target : {"/.hidden",
                                   "/../index.html",
                                   "/%2e%2e/index.html",
                                   "//app.js",
                                   "/missing"}) {
            EXPECT(fixture.request(http::verb::get, target).result() ==
                   http::status::not_found);
        }
    }
    fs::remove_all(dir);
}
TEST_SUIT_END
