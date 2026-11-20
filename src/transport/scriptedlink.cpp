#include "scriptedlink.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace {
constexpr char kPrompt[] = "\x1b[1;32mmatterm\x1b[0m:\x1b[1;34m~\x1b[0m$ ";
constexpr std::size_t kPromptWidth = 11; // "matterm:~$ " as displayed.

bool isContinuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}
} // namespace

Script &Script::output(std::string text, int bytesPerSecond) {
    Step step;
    step.kind = Step::Kind::Output;
    step.text = std::move(text);
    step.bytesPerSecond = bytesPerSecond;
    steps_.push_back(std::move(step));
    return *this;
}

Script &Script::delay(ms duration) {
    Step step;
    step.kind = Step::Kind::Delay;
    step.duration = duration;
    steps_.push_back(std::move(step));
    return *this;
}

Script &Script::waitForInput(std::string literal) {
    Step step;
    step.kind = Step::Kind::WaitLiteral;
    step.text = std::move(literal);
    steps_.push_back(std::move(step));
    return *this;
}

Script &Script::waitForInput(Predicate predicate) {
    Step step;
    step.kind = Step::Kind::WaitPredicate;
    step.predicate = std::move(predicate);
    steps_.push_back(std::move(step));
    return *this;
}

Script &Script::exit(int code) {
    Step step;
    step.kind = Step::Kind::Exit;
    step.code = code;
    steps_.push_back(std::move(step));
    return *this;
}

ScriptedLink::ScriptedLink(Config config,
                           Script script,
                           std::unique_ptr<Program> program)
    : config_(std::move(config))
    , script_(std::move(script))
    , program_(std::move(program)) {}

std::unique_ptr<ScriptedLink> ScriptedLink::simulator(Config config) {
    auto program = std::make_unique<Simulator>(config.columns, config.rows);
    return std::make_unique<ScriptedLink>(
        std::move(config), Script(), std::move(program));
}

void ScriptedLink::setEvents(Events events) {
    events_ = std::move(events);
}

bool ScriptedLink::sendInput(std::string_view bytes) {
    if (state_ != State::Running ||
        input_.size() + bytes.size() > config_.maxPendingInput) {
        return false;
    }
    input_.append(bytes);
    inputPending_ = true;
    return true;
}

void ScriptedLink::resize(int columns, int rows, int, int) {
    if (state_ != State::Running) {
        return;
    }
    config_.columns = columns;
    config_.rows = rows;
    if (program_) {
        program_->resize(columns, rows);
    }
}

void ScriptedLink::close() {
    if (state_ == State::Running) {
        state_ = State::Closed;
        input_.clear();
    }
}

ScriptedLink::State ScriptedLink::state() const noexcept {
    return state_;
}

int ScriptedLink::columns() const noexcept {
    return config_.columns;
}

int ScriptedLink::rows() const noexcept {
    return config_.rows;
}

void ScriptedLink::emit(std::string_view bytes) {
    if (!bytes.empty() && events_.output) {
        events_.output(bytes);
    }
}

void ScriptedLink::finish(int code) {
    state_ = State::Exited;
    input_.clear();
    if (events_.exited) {
        events_.exited(code, 0);
    }
}

void ScriptedLink::apply(Reaction reaction) {
    emit(reaction.output);
    if (reaction.exitCode && state_ == State::Running) {
        finish(*reaction.exitCode);
    }
}

void ScriptedLink::tick() {
    // Callbacks may call sendInput/resize/close, but not tick().
    if (state_ != State::Running || ticking_) {
        return;
    }
    ticking_ = true;
    const auto now = config_.clock();
    if (!started_) {
        started_ = true;
        stepTime_ = now;
    }
    while (state_ == State::Running && step_ < script_.steps().size() &&
           runStep(now)) {
        ++step_;
        sent_ = 0;
    }
    if (state_ == State::Running && step_ >= script_.steps().size()) {
        runProgram(now);
    }
    inputPending_ = false;
    ticking_ = false;
}

// Returns true when the current step is complete.
bool ScriptedLink::runStep(protocol::Time now) {
    const auto &step = script_.steps()[step_];
    using Kind = Script::Step::Kind;
    switch (step.kind) {
    case Kind::Output: {
        if (step.bytesPerSecond <= 0) {
            emit(step.text);
            return true;
        }
        const long long elapsed =
            std::max<long long>(0, (now - stepTime_).count());
        const auto allowed = std::size_t(std::min<long long>(
            elapsed * step.bytesPerSecond / 1000, (long long)step.text.size()));
        if (allowed > sent_) {
            emit(std::string_view(step.text).substr(sent_, allowed - sent_));
            sent_ = allowed;
        }
        if (sent_ < step.text.size()) {
            return false;
        }
        // Later steps start when the last byte was due, not when we noticed.
        stepTime_ +=
            protocol::Time((step.text.size() * 1000 + step.bytesPerSecond - 1) /
                           step.bytesPerSecond);
        return true;
    }
    case Kind::Delay:
        if (now - stepTime_ < step.duration) {
            return false;
        }
        stepTime_ += step.duration;
        return true;
    case Kind::WaitLiteral: {
        std::size_t end = 0;
        if (step.text.empty()) {
            end = input_.size();
            if (end == 0) {
                return false;
            }
        }
        else {
            const auto at = input_.find(step.text);
            if (at == std::string::npos) {
                return false;
            }
            end = at + step.text.size();
        }
        input_.erase(0, end);
        stepTime_ = std::max(stepTime_, now);
        return true;
    }
    case Kind::WaitPredicate:
        if (input_.empty() || !step.predicate(input_)) {
            return false;
        }
        input_.clear();
        stepTime_ = std::max(stepTime_, now);
        return true;
    case Kind::Exit:
        finish(step.code);
        return false; // State changed; the loop ends.
    }
    return false;
}

void ScriptedLink::runProgram(protocol::Time now) {
    if (!program_) {
        input_.clear(); // Nobody is listening.
        return;
    }
    if (!programStarted_) {
        programStarted_ = true;
        apply(program_->start());
    }
    if (state_ == State::Running && !input_.empty()) {
        const std::string bytes = std::move(input_);
        input_.clear();
        apply(program_->input(bytes));
    }
    if (state_ == State::Running) {
        apply(program_->tick(now));
    }
}

std::optional<protocol::Time> ScriptedLink::nextDeadline() const {
    if (state_ != State::Running) {
        return std::nullopt;
    }
    const auto now = config_.clock();
    if (!started_) {
        return now;
    }
    if (step_ >= script_.steps().size()) {
        if (program_ && (!programStarted_ || inputPending_)) {
            return now;
        }
        return program_ ? program_->nextDeadline() : std::nullopt;
    }
    const auto &step = script_.steps()[step_];
    using Kind = Script::Step::Kind;
    switch (step.kind) {
    case Kind::Output:
        if (step.bytesPerSecond <= 0) {
            return now;
        }
        // The next byte is due after ceil((sent + 1) / rate) seconds.
        return stepTime_ +
               protocol::Time(((sent_ + 1) * 1000 + step.bytesPerSecond - 1) /
                              step.bytesPerSecond);
    case Kind::Delay:
        return stepTime_ + step.duration;
    case Kind::WaitLiteral:
    case Kind::WaitPredicate:
        return inputPending_ ? std::optional<protocol::Time>(now)
                             : std::nullopt;
    case Kind::Exit:
        return now;
    }
    return std::nullopt;
}

Simulator::Simulator(int columns, int rows)
    : columns_(columns)
    , rows_(rows) {}

void Simulator::resize(int columns, int rows) {
    columns_ = columns;
    rows_ = rows;
}

ScriptedLink::Reaction Simulator::start() {
    return {std::string("\x1b[1mmatterm simulator\x1b[0m. Type 'help'.\r\n") +
                kPrompt,
            std::nullopt};
}

ScriptedLink::Reaction Simulator::input(std::string_view bytes) {
    ScriptedLink::Reaction reaction;
    for (const unsigned char byte : bytes) {
        accept(byte, reaction);
        if (reaction.exitCode) {
            break;
        }
    }
    return reaction;
}

std::size_t Simulator::limit() const {
    // Keep the line on one row so that Backspace never has to unwrap.
    return std::size_t(std::max<long long>(
        1, (long long)columns_ - (long long)kPromptWidth - 1));
}

void Simulator::accept(unsigned char byte, ScriptedLink::Reaction &reaction) {
    std::string &out = reaction.output;
    // Skip escape sequences (arrow keys, terminal replies, paste markers).
    switch (escape_) {
    case Escape::Introduced:
        escape_ = byte == '['   ? Escape::Csi
                  : byte == 'O' ? Escape::Ss3
                  : (byte == ']' || byte == 'P' || byte == '_' || byte == '^' ||
                     byte == 'X')
                      ? Escape::String
                      : Escape::None;
        return;
    case Escape::Csi:
        if (byte >= 0x40 && byte <= 0x7e) {
            escape_ = Escape::None;
        }
        return;
    case Escape::Ss3:
        escape_ = Escape::None;
        return;
    case Escape::String:
        if (byte == 0x07) {
            escape_ = Escape::None;
        }
        else if (byte == 0x1b) {
            escape_ = Escape::StringEscape;
        }
        return;
    case Escape::StringEscape:
        escape_ = byte == 0x1b ? Escape::StringEscape : Escape::None;
        return;
    case Escape::None:
        break;
    }
    const bool lineFeedOfCrLf = byte == '\n' && afterCr_;
    afterCr_ = byte == '\r';
    if (lineFeedOfCrLf) {
        return;
    }
    switch (byte) {
    case 0x1b:
        escape_ = Escape::Introduced;
        break;
    case '\r':
    case '\n':
        out += "\r\n";
        submit(reaction);
        break;
    case 0x7f:
    case 0x08:
        backspace(out);
        break;
    case 0x03: // Ctrl-C
        out += "^C\r\n";
        line_.clear();
        length_ = 0;
        out += kPrompt;
        break;
    case 0x15: // Ctrl-U
        while (length_ > 0) {
            backspace(out);
        }
        break;
    case 0x04: // Ctrl-D
        if (line_.empty()) {
            out += "logout\r\n";
            reaction.exitCode = 0;
        }
        break;
    default:
        if (byte >= 0x20 && byte != 0x7f) {
            insert(byte, out);
        }
        break;
    }
}

void Simulator::insert(unsigned char byte, std::string &out) {
    if (isContinuation(byte)) {
        if (skipping_ || line_.empty()) {
            return;
        }
    }
    else if (length_ >= limit()) {
        skipping_ = true;
        return;
    }
    else {
        skipping_ = false;
        ++length_;
    }
    line_.push_back(char(byte));
    out.push_back(char(byte));
}

void Simulator::backspace(std::string &out) {
    if (line_.empty()) {
        return;
    }
    while (!line_.empty() && isContinuation((unsigned char)line_.back())) {
        line_.pop_back();
    }
    if (!line_.empty()) {
        line_.pop_back();
    }
    --length_;
    out += "\b \b";
}

void Simulator::submit(ScriptedLink::Reaction &reaction) {
    const std::string line = std::move(line_);
    line_.clear();
    length_ = 0;
    skipping_ = false;
    reaction.output += command(line, reaction.exitCode);
    if (!reaction.exitCode) {
        reaction.output += kPrompt;
    }
}

std::string Simulator::colors() const {
    std::string out;
    for (int i = 0; i < 8; ++i) {
        out += "\x1b[4" + std::to_string(i) + "m  ";
    }
    out += "\x1b[0m\r\n";
    for (int i = 0; i < 8; ++i) {
        out += "\x1b[10" + std::to_string(i) + "m  ";
    }
    out += "\x1b[0m\r\n";
    const int cells = std::clamp(columns_, 2, 64);
    for (int i = 0; i < cells; ++i) {
        const int level = 255 * i / (cells - 1);
        out += "\x1b[48;2;" + std::to_string(level) + ";" +
               std::to_string(255 - level) + ";160m ";
    }
    out += "\x1b[0m\r\n"
           "\x1b[1mbold\x1b[0m \x1b[2mfaint\x1b[0m \x1b[3mitalic\x1b[0m "
           "\x1b[4munderline\x1b[0m \x1b[7minverse\x1b[0m\r\n";
    return out;
}

std::string Simulator::command(const std::string &line,
                               std::optional<int> &exitCode) const {
    std::istringstream words(line);
    std::string name;
    if (!(words >> name)) {
        return {};
    }
    // Everything after the command word, without surrounding blanks.
    std::string rest;
    std::getline(words >> std::ws, rest);
    while (!rest.empty() && rest.back() == ' ') {
        rest.pop_back();
    }
    if (name == "help") {
        return "Commands:\r\n"
               "  help        this list\r\n"
               "  echo TEXT   print TEXT\r\n"
               "  clear       clear the screen\r\n"
               "  colors      show colors and text attributes\r\n"
               "  size        show the window size in columns x rows\r\n"
               "  exit [N]    end the session with exit code N\r\n";
    }
    if (name == "echo") {
        return rest + "\r\n";
    }
    if (name == "clear") {
        return "\x1b[H\x1b[2J";
    }
    if (name == "colors") {
        return colors();
    }
    if (name == "size") {
        return std::to_string(columns_) + "x" + std::to_string(rows_) + "\r\n";
    }
    if (name == "exit") {
        int code = 0;
        if (!rest.empty()) {
            const auto end = rest.data() + rest.size();
            const auto result = std::from_chars(rest.data(), end, code);
            if (result.ec != std::errc() || result.ptr != end) {
                exitCode = 2;
                return "exit\r\nmatterm: exit: " + rest +
                       ": numeric argument required\r\n";
            }
        }
        exitCode = code & 255;
        return "exit\r\n";
    }
    return "matterm: " + name + ": command not found\r\n";
}
