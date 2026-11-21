#include "clientsession.h"
#include "message.h"
#include "mls-unit-test/unittest.h"
#include "terminalview.h"

#include <vector>

using namespace protocol;
using frontend::TerminalView;
using std::chrono::milliseconds;

namespace {
const std::string idA = "0123456789abcdef0123456789abcdef";
const std::string idB = "fedcba9876543210fedcba9876543210";

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
    std::string inputs() const {
        std::string all;
        for (const auto &message : messages()) {
            if (const auto *input = std::get_if<Input>(&message)) {
                all += input->data;
            }
        }
        return all;
    }
    std::uint64_t lastAck() const {
        std::uint64_t result = 0;
        for (const auto &message : messages()) {
            if (const auto *ack = std::get_if<Ack>(&message)) {
                result = ack->consumedOffset;
            }
        }
        return result;
    }
    std::vector<std::string> sent;
    std::vector<int> closeCodes;
    bool open = false;

private:
    Events events_;
};

// One browser view: a channel, a session on it and the view they feed. Declared
// in this order so the view goes away before the link it listens to.
struct Slot {
    FakeChannel channel;
    milliseconds now{1000};
    std::unique_ptr<TerminalView> view;
    std::unique_ptr<ClientSession> session;
    int changes = 0;

    explicit Slot(std::string sessionId = {},
                  std::uint64_t resumeOffset = 0,
                  std::size_t columns = 20,
                  std::size_t rows = 5) {
        TerminalView::Config viewConfig;
        viewConfig.columns = columns;
        viewConfig.rows = rows;
        view = std::make_unique<TerminalView>(viewConfig);
        ClientSession::Config config;
        config.clock = [this] { return now; };
        view->configure(config, std::move(sessionId), resumeOffset);
        session = std::make_unique<ClientSession>(channel, config);
        view->attach(*session, [this] { return session->errorCode(); });
        view->setChanged([this] { ++changes; });
    }
    ~Slot() {
        view.reset();
        session.reset();
    }
    void connect(const std::string &id = idA) {
        channel.opened();
        Welcome welcome;
        welcome.sessionId = id;
        channel.deliver(welcome);
    }
    void output(std::string_view text) {
        Output message;
        message.offset = session->receivedOffset();
        message.data = std::string(text);
        channel.deliver(message);
    }
    void advance(int ms) {
        now += milliseconds(ms);
        session->tick();
    }
    std::string row(std::size_t index) const {
        std::string text;
        for (std::size_t column = 0; column < view->model().columns();
             ++column) {
            const auto code = view->model().textCodePoint(
                {view->model().firstRetainedRow() +
                     view->model().historySize() + index,
                 column});
            text += code && *code >= 32 && *code < 127 ? char(*code) : ' ';
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }
    std::string screen() const {
        std::string all;
        for (std::size_t i = 0; i < view->model().rows(); ++i) {
            all += row(i) + "\n";
        }
        return all;
    }
};
bool contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_SUIT_BEGIN(TerminalView)

TEST_CASE("Output reaches only the view of its own session") {
    Slot a, b;
    a.connect();
    b.connect(idB);
    a.output("alpha");
    b.output("beta");
    EXPECT_EQ(a.row(0), "alpha");
    EXPECT_EQ(b.row(0), "beta");
    EXPECT(!contains(a.screen(), "beta"));
    EXPECT(!contains(b.screen(), "alpha"));
    EXPECT_EQ(a.session->receivedOffset(), std::uint64_t(5));
    EXPECT_EQ(b.session->receivedOffset(), std::uint64_t(4));
}

TEST_CASE("Replies, titles and palette changes stay inside their view") {
    Slot a, b;
    a.connect();
    b.connect(idB);
    // A cursor position report goes back through A's link only.
    a.output("\x1b[6n");
    EXPECT_EQ(a.channel.inputs(), "\x1b[1;1R");
    EXPECT_EQ(b.channel.inputs(), "");
    a.output("\x1b]0;title of A\x07");
    EXPECT_EQ(a.view->title(), "title of A");
    EXPECT_EQ(b.view->title(), "");
    // OSC 4 changes A's palette and leaves B's alone.
    const auto before = b.view->palette().colors[1];
    a.output("\x1b]4;1;rgb:12/34/56\x07");
    EXPECT(a.view->palette().colors[1].r == 0x12);
    EXPECT(b.view->palette().colors[1].r == before.r);
    a.output("\x1b]104\x07");
    EXPECT(a.view->palette().colors[1].r == before.r);
    // Input is sent through the view that is asked, nowhere else.
    EXPECT(a.view->sendInput("ls\r"));
    EXPECT_EQ(a.channel.inputs(), "\x1b[1;1Rls\r");
    EXPECT_EQ(b.channel.inputs(), "");
}

TEST_CASE("Selection and glide belong to the view") {
    Slot a, b;
    a.connect();
    b.connect(idB);
    a.output("hello");
    a.view->selection = frontend::Selection::all(a.view->model());
    EXPECT(a.view->selection.has_value());
    EXPECT(!b.view->selection.has_value());
}

TEST_CASE("A background view parses and acknowledges without being drawn") {
    Slot active, background;
    active.connect();
    background.connect(idB);
    // A lot of output, in several messages, to a view nobody looks at.
    const std::string line(18, 'x');
    std::uint64_t total = 0;
    for (int i = 0; i < 40; ++i) {
        background.output(line + "\r\n");
        total += line.size() + 2;
    }
    background.output("LAST");
    total += 4;
    EXPECT(contains(background.screen(), "LAST"));
    EXPECT_EQ(background.session->receivedOffset(), total);
    // Everything is acknowledged without anyone asking for a frame.
    background.advance(200);
    EXPECT_EQ(background.channel.lastAck(), total);
    EXPECT(background.changes > 0);
    EXPECT_EQ(active.changes, 0);
    EXPECT_EQ(active.channel.lastAck(), std::uint64_t(0));
}

TEST_CASE("Reload attaches by id and rebuilds from a reset") {
    Slot slot(idA);
    slot.channel.opened();
    const auto hello = std::get<Hello>(slot.channel.messages().at(0));
    EXPECT_EQ(hello.sessionId, idA);
    EXPECT_EQ(hello.resumeOffset, std::uint64_t(0));
    EXPECT((hello.features & featureReplay) != 0);
    // Stale local state is cleared before the first replayed byte.
    slot.view->model().resize(20, 5);
    slot.view->selection = frontend::Selection::all(slot.view->model());
    Welcome welcome;
    welcome.sessionId = idA;
    welcome.flags = 3;
    welcome.features = featureReplay;
    slot.channel.deliver(welcome);
    EXPECT(!slot.view->selection.has_value());
    slot.output("replayed");
    EXPECT_EQ(slot.row(0), "replayed");
    EXPECT(slot.view->state() == TerminalView::State::Live);
}

TEST_CASE("A reset clears screen, history, palette, title and selection") {
    Slot slot;
    slot.connect();
    for (int i = 0; i < 30; ++i) {
        slot.output("line " + std::to_string(i) + "\r\n");
    }
    slot.output("\x1b]0;old\x07\x1b]4;1;rgb:12/34/56\x07");
    EXPECT(slot.view->model().historySize() > 0);
    slot.view->selection = frontend::Selection::all(slot.view->model());
    slot.view->resetForReplay();
    EXPECT_EQ(slot.view->model().historySize(), std::size_t(0));
    EXPECT_EQ(slot.screen().find("line"), std::string::npos);
    EXPECT_EQ(slot.view->title(), "");
    EXPECT(!slot.view->selection.has_value());
    EXPECT(slot.view->palette().colors[1].r != 0x12);
    EXPECT(slot.view->state() == TerminalView::State::Live);
}

TEST_CASE("A process exit ends the view and says so") {
    Slot slot;
    slot.connect();
    slot.output("bye");
    Exit exit;
    exit.code = 7;
    slot.channel.deliver(exit);
    EXPECT(slot.view->state() == TerminalView::State::Exited);
    EXPECT_EQ(slot.view->exitCode(), 7);
    EXPECT(slot.view->ended());
    EXPECT(contains(slot.screen(), "process exited: 7"));
    EXPECT(!slot.view->sendInput("x"));
    EXPECT(!slot.view->reconnectAllowed());
}

TEST_CASE(
    "A taken-over view stays closed and never asks for the session back") {
    Slot slot(idA);
    slot.channel.opened();
    Welcome welcome;
    welcome.sessionId = idA;
    welcome.flags = 3;
    welcome.features = featureReplay;
    slot.channel.deliver(welcome);
    slot.output("working");
    const auto sentBefore = slot.channel.sent.size();
    slot.channel.deliver(Error{ErrorCode::Replaced, "session taken over"});
    EXPECT(slot.view->state() == TerminalView::State::Replaced);
    EXPECT(contains(slot.screen(), "taken over"));
    EXPECT(!slot.view->reconnectAllowed());
    EXPECT(!slot.view->sendInput("x"));
    // The transport closing afterwards changes nothing, and nothing is sent.
    slot.channel.lost(1008);
    slot.advance(60000);
    EXPECT(slot.view->state() == TerminalView::State::Replaced);
    EXPECT_EQ(slot.channel.sent.size(), sentBefore);
    EXPECT(slot.channel.closeCodes.size() <= 1);
    // The old content stays visible next to the notice.
    EXPECT(contains(slot.screen(), "working"));
}

TEST_CASE("An unavailable id is reported, never replaced by a new shell") {
    Slot slot(idA);
    slot.channel.opened();
    slot.channel.deliver(
        Error{ErrorCode::ResumeUnsupported, "session unavailable"});
    EXPECT(slot.view->state() == TerminalView::State::Unavailable);
    EXPECT(contains(slot.screen(), "session unavailable"));
    EXPECT(!slot.view->reconnectAllowed());
    EXPECT(!slot.view->sendInput("x"));
    EXPECT(slot.session->state() == ClientSession::State::Failed);
}

TEST_CASE("A lost connection is a disconnect that may be retried") {
    Slot slot;
    slot.connect();
    slot.output("data");
    slot.channel.lost(1006);
    EXPECT(slot.view->state() == TerminalView::State::Disconnected);
    EXPECT(slot.view->reconnectAllowed());
    EXPECT(contains(slot.screen(), "disconnected"));
    EXPECT(!slot.view->sendInput("x"));
    // Other server errors are failures too, but not a take-over.
    Slot limited;
    limited.channel.opened();
    limited.channel.deliver(Error{ErrorCode::RateLimited, "slow down"});
    EXPECT(limited.view->state() == TerminalView::State::Disconnected);
}

TEST_CASE("A reconnect may keep the parsed state and resume from an offset") {
    Slot first;
    first.connect();
    first.output("kept on screen");
    const auto offset = first.session->receivedOffset();
    first.channel.lost(1006);
    EXPECT(first.view->reconnectAllowed());
    // A new session continues from the retained offset; the screen is not
    // reset.
    FakeChannel channel;
    milliseconds now{5000};
    ClientSession::Config config;
    config.clock = [&] { return now; };
    first.view->configure(config, idA, offset);
    ClientSession session(channel, config);
    first.view->attach(session);
    channel.opened();
    const auto hello = std::get<Hello>(channel.messages().at(0));
    EXPECT_EQ(hello.sessionId, idA);
    EXPECT_EQ(hello.resumeOffset, offset);
    Welcome welcome;
    welcome.sessionId = idA;
    welcome.startOffset = offset;
    welcome.flags = 1;
    welcome.features = featureReplay;
    channel.deliver(welcome);
    EXPECT(contains(first.screen(), "kept on screen"));
    EXPECT(first.view->state() == TerminalView::State::Live);
    first.view->attach(*first.session); // Leave nothing pointing at `session`.
}

TEST_CASE(
    "A link that could not be created is a disconnect that may be retried") {
    Slot slot("", 0, 80, 5);
    slot.view->detach();
    slot.view->linkFailed("cannot open the connection");
    EXPECT(slot.view->state() == TerminalView::State::Disconnected);
    EXPECT(slot.view->reconnectAllowed());
    EXPECT(contains(slot.screen(), "cannot open the connection"));
    // The first reason stands.
    slot.view->linkFailed("again");
    EXPECT(!contains(slot.screen(), "again"));
}

TEST_CASE("The owner can leave a notice in the terminal") {
    Slot slot("", 0, 80, 5);
    slot.connect();
    slot.output("text");
    slot.view->note("press Enter to close");
    EXPECT(contains(slot.screen(), "[press Enter to close]"));
    EXPECT(contains(slot.screen(), "text"));
    EXPECT(slot.view->state() == TerminalView::State::Live);
}

TEST_CASE("The terminal modes are those of this view's own program") {
    Slot a, b;
    a.connect();
    b.connect(idB);
    a.output("\x1b[?1000h");
    EXPECT(a.view->modes().mouseTracking != TerminalModes::MouseTracking::Off);
    EXPECT(b.view->modes().mouseTracking == TerminalModes::MouseTracking::Off);
}

TEST_CASE("Resizing the view resizes the model and tells the link") {
    Slot slot;
    slot.connect();
    slot.view->resize(40, 10);
    EXPECT_EQ(slot.view->model().columns(), std::size_t(40));
    EXPECT_EQ(slot.view->model().rows(), std::size_t(10));
    bool sawResize = false;
    for (const auto &message : slot.channel.messages()) {
        if (const auto *resize = std::get_if<Resize>(&message)) {
            sawResize = resize->columns == 40 && resize->rows == 10;
        }
    }
    EXPECT(sawResize);
}

TEST_SUIT_END
