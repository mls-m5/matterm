#pragma once

#include "protocoltime.h"
#include "terminallink.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A list of steps a ScriptedLink plays, like a terminal program's behavior
// without the program. Build it with chained calls:
//   Script().output("login: ").waitForInput("\r").output("ok\r\n").exit(0)
class Script {
public:
    using ms = std::chrono::milliseconds;
    using Predicate = std::function<bool(std::string_view input)>;
    struct Step {
        enum class Kind { Output, Delay, WaitLiteral, WaitPredicate, Exit };
        Kind kind = Kind::Output;
        std::string text;       // Output text or the literal to wait for.
        int bytesPerSecond = 0; // Output only; 0 means all at once.
        ms duration{0};
        Predicate predicate;
        int code = 0;
    };

    // Terminal output. With bytesPerSecond > 0 the text streams at that rate.
    Script &output(std::string text, int bytesPerSecond = 0);
    Script &delay(ms duration);
    // Pause until the input received so far contains `literal`. Input up to
    // and including the match is consumed; later bytes stay queued.
    Script &waitForInput(std::string literal);
    // Pause until the predicate accepts the input received since the previous
    // wait; all of it is then consumed.
    Script &waitForInput(Script::Predicate predicate);
    // End the session with this exit code (reported once).
    Script &exit(int code);

    const std::vector<Step> &steps() const noexcept {
        return steps_;
    }

private:
    std::vector<Step> steps_;
};

// TerminalLink without a process or a network: plays a Script and then,
// optionally, hands the input to an interactive Program. Deterministic: time
// comes from the injected clock, nothing sleeps, and all callbacks run inside
// tick(), never inside sendInput() or resize(), so a reply written while the
// parser consumes output cannot re-enter the link.
class ScriptedLink final : public TerminalLink {
public:
    struct Config {
        std::function<protocol::Time()> clock; // Required.
        int columns = 80;
        int rows = 24;
        std::size_t maxPendingInput = 65536; // Typeahead limit in bytes.
    };

    // What an interactive program does in response to something.
    struct Reaction {
        std::string output;
        std::optional<int> exitCode; // Set to end the session after `output`.
    };
    // A reactive program that takes over once the script has finished. Pure:
    // it returns output instead of writing it.
    class Program {
    public:
        virtual ~Program() = default;
        virtual Reaction start() = 0;
        virtual Reaction input(std::string_view bytes) = 0;
        virtual void resize(int columns, int rows) = 0;
    };
    enum class State { Running, Exited, Closed };

    ScriptedLink(Config config,
                 Script script,
                 std::unique_ptr<Program> program = nullptr);
    // A link running the interactive Simulator (see below) with no script.
    static std::unique_ptr<ScriptedLink> simulator(Config config);

    void setEvents(Events events) override;
    // Queues the bytes; false after the program exited or the link was closed,
    // or when more than maxPendingInput bytes are waiting.
    bool sendInput(std::string_view bytes) override;
    void resize(int columns,
                int rows,
                int pixelWidth = 0,
                int pixelHeight = 0) override;
    // Ends the session without events.
    void close() override;

    // Advance the script to the injected clock's current time.
    void tick();
    // When tick() next has something to do; empty when only input can wake it.
    std::optional<protocol::Time> nextDeadline() const;
    State state() const noexcept;
    int columns() const noexcept;
    int rows() const noexcept;

private:
    bool runStep(protocol::Time now);
    void runProgram();
    void apply(Reaction reaction);
    void emit(std::string_view bytes);
    void finish(int code);

    Config config_;
    Script script_;
    std::unique_ptr<Program> program_;
    Events events_;
    State state_ = State::Running;
    bool started_ = false;
    bool programStarted_ = false;
    bool ticking_ = false;
    bool inputPending_ = false;
    std::size_t step_ = 0;
    std::size_t sent_ = 0; // Bytes of a paced output step already emitted.
    protocol::Time stepTime_{};
    std::string input_;
};

// The interactive program behind ScriptedLink::simulator(): a prompt, a line
// editor (typed text, Backspace, Enter, Ctrl-C/U/D) and the commands help,
// echo, clear, colors, size and exit [n]. It speaks plain VT output and
// ignores escape sequences in its input, such as terminal replies.
class Simulator final : public ScriptedLink::Program {
public:
    Simulator(int columns, int rows);
    ScriptedLink::Reaction start() override;
    ScriptedLink::Reaction input(std::string_view bytes) override;
    void resize(int columns, int rows) override;

private:
    enum class Escape { None, Introduced, Csi, Ss3, String, StringEscape };
    void accept(unsigned char byte, ScriptedLink::Reaction &reaction);
    void insert(unsigned char byte, std::string &out);
    void backspace(std::string &out);
    void submit(ScriptedLink::Reaction &reaction);
    std::string command(const std::string &line,
                        std::optional<int> &exitCode) const;
    std::size_t limit() const;
    std::string colors() const;

    int columns_;
    int rows_;
    std::string line_;
    std::size_t length_ = 0; // Code points in line_.
    bool skipping_ = false;  // Dropping a code point that did not fit.
    bool afterCr_ = false;
    Escape escape_ = Escape::None;
};
