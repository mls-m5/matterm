#pragma once

// Adapter that runs a fakeos::System as a ScriptedLink program. Header only, so
// the fakeos library itself does not depend on the transport layer.

#include "scriptedlink.h"
#include "system.h"

#include <deque>
#include <memory>
#include <string>

namespace fakeos {

class ScriptedProgram final : public ScriptedLink::Program {
public:
    explicit ScriptedProgram(Config config)
        : system_(std::move(config)) {}

    ScriptedLink::Reaction start() override {
        return convert(system_.start());
    }
    ScriptedLink::Reaction input(std::string_view bytes) override {
        return convert(system_.input(bytes));
    }
    void resize(int columns, int rows) override {
        system_.resize(columns, rows);
    }
    ScriptedLink::Reaction tick(protocol::Time now) override {
        // Chat events from the host's connection are handled at the next tick
        // (a link may not be re-entered from outside its callbacks).
        Reaction total;
        while (!chatEvents_.empty()) {
            const auto event = std::move(chatEvents_.front());
            chatEvents_.pop_front();
            Reaction r = event.kind == 0   ? system_.chatOpened()
                         : event.kind == 1 ? system_.chatLine(event.data)
                                           : system_.chatClosed();
            total.output += r.output;
            if (r.exitCode) {
                total.exitCode = r.exitCode;
            }
        }
        Reaction r = system_.tick(now);
        total.output += r.output;
        if (r.exitCode) {
            total.exitCode = r.exitCode;
        }
        return convert(std::move(total));
    }
    std::optional<protocol::Time> nextDeadline() const override {
        if (!chatEvents_.empty()) {
            return protocol::Time{}; // Now.
        }
        return system_.nextDeadline();
    }
    // The chat connection reports from outside the link (a JavaScript
    // WebSocket): kind 0 opened, 1 a text frame, 2 closed.
    void chatEvent(int kind, std::string data) {
        chatEvents_.push_back({kind, std::move(data)});
    }
    System &system() {
        return system_;
    }

private:
    static ScriptedLink::Reaction convert(Reaction reaction) {
        return {std::move(reaction.output), reaction.exitCode};
    }
    struct ChatEvent {
        int kind;
        std::string data;
    };
    System system_;
    std::deque<ChatEvent> chatEvents_;
};

} // namespace fakeos
