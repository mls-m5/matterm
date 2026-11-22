#include "daemonlink.h"
#include "message.h"
#include "mls-unit-test/unittest.h"

#include <chrono>
#include <vector>

using namespace protocol;
using frontend::DaemonLink;
using frontend::Reattach;
using frontend::TerminalView;
using std::chrono::milliseconds;

namespace {
const std::string idA = "0123456789abcdef0123456789abcdef";

class FakeChannel : public MessageChannel {
public:
    void setEvents(Events events) override {
        events_ = std::move(events);
    }
    bool send(std::string_view bytes) override {
        if (!open) {
            return false;
        }
        sent.emplace_back(bytes);
        return true;
    }
    void close(int code) override {
        closeCodes.push_back(code);
        open = false;
    }
    void opened() {
        open = true;
        events_.opened();
    }
    void deliver(const ServerMessage &message) {
        std::string bytes;
        encode(message, bytes);
        events_.message(bytes);
    }
    void lost(int code) {
        open = false;
        events_.closed(code);
    }
    std::vector<ClientMessage> messages() const {
        std::vector<ClientMessage> result;
        for (const auto &bytes : sent) {
            const auto decoded = decodeClient(bytes);
            result.push_back(decoded ? *decoded.message
                                     : ClientMessage(Close{}));
        }
        return result;
    }
    std::vector<std::string> sent;
    std::vector<int> closeCodes;
    bool open = false;

private:
    Events events_;
};

// A pane: the link first, the view last, so the view goes away before what it
// listens to.
struct Pane {
    FakeChannel *channel = nullptr; // Owned by the link.
    milliseconds now{1000};
    int pumped = 0;
    std::unique_ptr<DaemonLink> link;
    std::unique_ptr<TerminalView> view;

    explicit Pane(std::string sessionId = {}, std::uint64_t offset = 0) {
        TerminalView::Config viewConfig;
        viewConfig.columns = 20;
        viewConfig.rows = 5;
        view = std::make_unique<TerminalView>(viewConfig);
        ClientSession::Config config;
        config.clock = [this] { return now; };
        view->configure(config, std::move(sessionId), offset);
        auto owned = std::make_unique<FakeChannel>();
        channel = owned.get();
        link = std::make_unique<DaemonLink>(
            std::move(owned), config, [this] { ++pumped; });
        view->attach(*link, [this] { return link->errorCode(); });
    }
    ~Pane() {
        view->detach();
    }
    void connect(const std::string &id = idA) {
        channel->opened();
        Welcome welcome;
        welcome.sessionId = id;
        channel->deliver(welcome);
    }
    void output(std::string_view text) {
        Output message;
        message.offset = link->receivedOffset();
        message.data = std::string(text);
        channel->deliver(message);
    }
};
} // namespace

TEST_SUIT_BEGIN(DaemonLink)

TEST_CASE("A new pane asks for a new terminal and learns its id") {
    Pane pane;
    pane.connect();
    const auto sent = pane.channel->messages();
    EXPECT(!sent.empty());
    const auto *hello = std::get_if<Hello>(&sent.front());
    EXPECT(hello != nullptr);
    EXPECT_EQ(pane.link->requestedId(), "");
    EXPECT_EQ(pane.link->sessionId(), idA);
    EXPECT_EQ(pane.link->knownId(), idA);
    EXPECT(pane.view->state() == TerminalView::State::Live);
}

TEST_CASE("Output, input and resize go through the link") {
    Pane pane;
    pane.connect();
    pane.output("hello");
    EXPECT_EQ(pane.link->receivedOffset(), std::uint64_t(5));
    EXPECT(pane.view->sendInput("ls\r"));
    bool input = false;
    for (const auto &message : pane.channel->messages()) {
        if (const auto *in = std::get_if<Input>(&message)) {
            input = in->data == "ls\r";
        }
    }
    EXPECT(input);
}

TEST_CASE("tick runs the pump before the session") {
    Pane pane;
    pane.link->tick();
    EXPECT_EQ(pane.pumped, 1);
}

TEST_CASE("Lost connection: Enter attaches the same session again") {
    Pane pane;
    pane.connect();
    pane.output("abc");
    pane.channel->lost(1006);
    EXPECT(pane.view->state() == TerminalView::State::Disconnected);
    EXPECT(frontend::reattachKind(*pane.view) == Reattach::Same);
    EXPECT_EQ(pane.link->knownId(), idA);
    EXPECT_EQ(pane.link->receivedOffset(), std::uint64_t(3));
}

TEST_CASE("Taken over by another window is also a same-session attach") {
    Pane pane;
    pane.connect();
    pane.channel->deliver(Error{ErrorCode::Replaced, "session taken over"});
    EXPECT(pane.view->state() == TerminalView::State::Replaced);
    EXPECT(frontend::reattachKind(*pane.view) == Reattach::Same);
}

TEST_CASE("A session the daemon does not know gives a fresh terminal") {
    Pane pane(idA);
    pane.channel->opened();
    pane.channel->deliver(
        Error{ErrorCode::ResumeUnsupported, "no such session"});
    EXPECT(pane.view->state() == TerminalView::State::Unavailable);
    EXPECT(frontend::reattachKind(*pane.view) == Reattach::Fresh);
    EXPECT_EQ(pane.link->requestedId(), idA);
}

TEST_CASE("A live pane or an ended program is not attached again") {
    Pane pane;
    pane.connect();
    EXPECT(frontend::reattachKind(*pane.view) == Reattach::None);
    pane.channel->deliver(Exit{0, 0});
    EXPECT(pane.view->state() == TerminalView::State::Exited);
    EXPECT(frontend::reattachKind(*pane.view) == Reattach::None);
    EXPECT(frontend::reattachToDaemon(
               *pane.view, pane.link.get(), "/nonexistent/sock", [] {
                   return milliseconds(0);
               }) == nullptr);
}

TEST_CASE("close ends the terminal, detach only the connection") {
    Pane closing;
    closing.connect();
    closing.link->close();
    bool closeSent = false;
    for (const auto &message : closing.channel->messages()) {
        closeSent = closeSent || std::holds_alternative<Close>(message);
    }
    EXPECT(closeSent);

    Pane leaving;
    leaving.connect();
    const auto before = leaving.channel->sent.size();
    leaving.link->detach();
    EXPECT_EQ(leaving.channel->sent.size(), before);
    EXPECT_EQ(leaving.channel->closeCodes.size(), std::size_t(1));
    EXPECT_EQ(leaving.channel->closeCodes.front(), 1000);
}

TEST_CASE("No daemon: the view says so and may retry") {
    TerminalView::Config viewConfig;
    viewConfig.columns = 20;
    viewConfig.rows = 5;
    TerminalView view(viewConfig);
    auto link =
        frontend::attachToDaemon(view, "/nonexistent/matterm.sock", {}, 0, [] {
            return milliseconds(0);
        });
    EXPECT(link == nullptr);
    EXPECT(view.state() == TerminalView::State::Disconnected);
    EXPECT(view.reconnectAllowed());
}

TEST_SUIT_END
