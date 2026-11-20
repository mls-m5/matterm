#pragma once

// Adapter that runs a fakeos::System as a ScriptedLink program. Header only, so
// the fakeos library itself does not depend on the transport layer.

#include "scriptedlink.h"
#include "system.h"

#include <memory>

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
        return convert(system_.tick(now));
    }
    std::optional<protocol::Time> nextDeadline() const override {
        return system_.nextDeadline();
    }
    System &system() {
        return system_;
    }

private:
    static ScriptedLink::Reaction convert(Reaction reaction) {
        return {std::move(reaction.output), reaction.exitCode};
    }
    System system_;
};

} // namespace fakeos
