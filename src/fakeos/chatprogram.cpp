// The chat as a program of the pretend system: the ChatClient and the inline
// view of src/chat, wired to the keyboard and to the host's connection
// (World::chat). It draws on the normal screen, so the terminal's own
// scrollback holds the conversation.

#include "chatclient.h"
#include "chatview.h"
#include "programs.h"

#include <algorithm>

namespace fakeos {
namespace {

class ChatProgram final : public Program {
public:
    bool inlineMode() const override {
        return true;
    }
    Time interval() const override {
        return Time(100);
    }
    void frame(World &, Time, AsciiCanvas &) override {} // rawFrame() draws.

    void begin(World &world, Time now) override {
        world_ = &world;
        const auto nowMs = chat::Millis(now.count());
        chat::ViewConfig view;
        view.caps.truecolor = world.caps.truecolor;
        view.caps.colors256 = world.caps.colors256;
        view.caps.unicode = world.caps.unicode;
        view.columns = world.columns;
        view.rows = world.rows;
        view.unixMsAtZero = world.unixNow() * 1000 - nowMs;
        view_ = std::make_unique<chat::ChatView>(view);
        chat::ChatClient::Config config;
        config.name = "guest-" + std::to_string(1000 + world.rng.below(9000));
        client_ = std::make_unique<chat::ChatClient>(config);
        apply(client_->connect(nowMs));
        world.chat.open();
        dirty_ = true;
    }

    void external(World &,
                  const std::string &kind,
                  std::string_view data,
                  Time now) override {
        if (!client_) {
            return;
        }
        const auto nowMs = chat::Millis(now.count());
        if (kind == "opened") {
            apply(client_->opened(nowMs));
        }
        else if (kind == "line") {
            apply(client_->receive(data, nowMs));
            // The client may have cut an acknowledged part off the draft.
            const std::string text = input_.text();
            if (text.rfind('/', 0) != 0 &&
                client_->state() == chat::ChatClient::State::Joined &&
                client_->draft() != text) {
                input_.set(client_->draft());
            }
        }
        else if (kind == "closed") {
            apply(client_->closed(nowMs));
        }
        dirty_ = true;
    }

    void resize(World &, int columns, int rows) override {
        if (view_) {
            pending_ += view_->resize(columns, rows);
        }
        dirty_ = true;
    }

    void key(World &, const Key &key) override {
        if (!client_ || done_) {
            return;
        }
        const auto nowMs = chat::Millis(lastNow_);
        bool changed = false;
        switch (key.type) {
        case Key::Type::Text:
            changed = input_.insertText(key.text);
            break;
        case Key::Type::Backspace:
            changed = input_.backspace();
            break;
        case Key::Type::Delete:
            changed = input_.del();
            break;
        case Key::Type::Left:
            input_.left();
            break;
        case Key::Type::Right:
            input_.right();
            break;
        case Key::Type::Home:
            input_.home();
            break;
        case Key::Type::End:
            input_.end();
            break;
        case Key::Type::Enter:
            enter(nowMs);
            dirty_ = true;
            return;
        case Key::Type::Ctrl:
            if (key.text == "c" || (key.text == "d" && input_.text().empty())) {
                leaving_ = true;
                apply(client_->leave(nowMs));
                dirty_ = true;
                return;
            }
            if (key.text == "u") {
                changed = input_.killLine();
            }
            else if (key.text == "w") {
                changed = input_.killWord();
            }
            else if (key.text == "a") {
                input_.home();
            }
            else if (key.text == "e") {
                input_.end();
            }
            break;
        default:
            break;
        }
        if (changed && client_->state() != chat::ChatClient::State::NeedName) {
            const std::string text = input_.text();
            chat::ChatClient::Out out;
            client_->edit(
                text.rfind('/', 0) == 0 ? std::string() : text, nowMs, out);
            apply(out);
        }
        dirty_ = true;
    }

    std::string rawFrame(World &, Time now) override {
        lastNow_ = now.count();
        const auto nowMs = chat::Millis(now.count());
        if (!client_ || done_) {
            return {};
        }
        apply(client_->tick(nowMs));
        if (const auto frame = view_->nextFrame(nowMs);
            frame && *frame <= nowMs) {
            dirty_ = true;
        }
        if (client_->state() == chat::ChatClient::State::Closed) {
            dirty_ = true;
        }
        std::string out = std::move(pending_);
        pending_.clear();
        if (dirty_) {
            dirty_ = false;
            out += view_->render(*client_, input_, nowMs);
        }
        if (client_->state() == chat::ChatClient::State::Closed) {
            done_ = true;
        }
        return out;
    }

    bool finished() const override {
        return done_;
    }

    std::string leave(World &) override {
        return view_ ? view_->finish() : std::string();
    }

    void end(World &world) override {
        if (!closedHost_ && world.chat.close) {
            world.chat.close();
        }
        closedHost_ = true;
    }

private:
    void apply(const chat::ChatClient::Out &out) {
        for (const auto &line : out.send) {
            world_->chat.send(line);
        }
        if (out.close && !closedHost_) {
            world_->chat.close();
            closedHost_ = true;
        }
    }

    void enter(chat::Millis nowMs) {
        const std::string text = input_.text();
        if (client_->state() == chat::ChatClient::State::NeedName) {
            apply(client_->hello(text, nowMs));
            input_.killLine();
            return;
        }
        if (text.rfind('/', 0) != 0) {
            apply(client_->submit(nowMs));
            return;
        }
        input_.killLine();
        const auto space = text.find(' ');
        const std::string verb = text.substr(0, space);
        const std::string arg =
            space == std::string::npos ? std::string() : text.substr(space + 1);
        if (verb == "/quit") {
            leaving_ = true;
            apply(client_->leave(nowMs));
        }
        else if (verb == "/nick") {
            const auto out = client_->rename(arg, nowMs);
            if (out.send.empty()) {
                view_->note("a name is 2-16 of A-Z a-z 0-9 _ -");
            }
            apply(out);
        }
        else if (verb == "/who") {
            std::string names;
            for (const auto &n : client_->users()) {
                names += (names.empty() ? "" : ", ") + n;
            }
            view_->note("here: " + names);
        }
        else if (verb == "/help") {
            view_->note("Enter sends at once, five quiet seconds send too. "
                        "Ctrl-C leaves.");
            view_->note("/nick NAME   /who   /quit   (everything you type is "
                        "seen live)");
        }
        else {
            view_->note("unknown command (try /help)");
        }
    }

    World *world_ = nullptr;
    std::unique_ptr<chat::ChatClient> client_;
    std::unique_ptr<chat::ChatView> view_;
    chat::ChatInput input_;
    std::string pending_;
    long long lastNow_ = 0;
    bool dirty_ = false, done_ = false, leaving_ = false, closedHost_ = false;
};

} // namespace

std::unique_ptr<Program> makeChat(const std::vector<std::string> &) {
    return std::make_unique<ChatProgram>();
}

} // namespace fakeos
