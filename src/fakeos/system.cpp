#include "system.h"

#include "eggs.h"

#include <algorithm>

namespace fakeos {
namespace {
const char *kEnterApp = "\x1b[?1049h\x1b[?25l\x1b[0m\x1b[2J";
const char *kLeaveApp = "\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l";
// Reveal pacing: a newline costs this many characters' time, so lines (not only
// characters) set the pace; even a one-line text takes a moment, so the effect
// is seen, but only a moment (it must never feel sluggish); and a long text is
// shown within a few seconds whatever the configured rate.
constexpr double kNewlineCost = 12;
constexpr double kLeastRevealSeconds = 0.35;
constexpr double kMostRevealSeconds = 4.0;
// While text is revealed there is a pip about this often: a short one for the
// last character shown, a lower, longer one when a line was completed.
constexpr int kBeepGapMs = 45;
// The start command typed for the visitor: a pause, then letters, then a pause
// before Enter.
constexpr int kAutoStartPauseMs = 350;
constexpr int kAutoTypeMs = 70;
constexpr int kAutoEnterPauseMs = 450;

// What showing one unit of terminal output costs, and how many bytes it takes:
// a character, a line end, or an escape sequence (free).
struct Unit {
    std::size_t bytes;
    double cost;
};
Unit nextUnit(const std::string &text, std::size_t at) {
    const auto b = (unsigned char)text[at];
    if (b == 0x1b) {
        std::size_t n = 1;
        if (at + 1 < text.size() && text[at + 1] == '[') {
            n = 2;
            while (at + n < text.size() &&
                   !((unsigned char)text[at + n] >= 0x40 &&
                     (unsigned char)text[at + n] <= 0x7e)) {
                ++n;
            }
            n = std::min(n + 1, text.size() - at);
        }
        else if (at + 1 < text.size() && text[at + 1] == ']') {
            n = 2;
            while (at + n < text.size() && text[at + n] != '\a' &&
                   text[at + n] != '\x1b') {
                ++n;
            }
            n = std::min(
                n + (at + n < text.size() && text[at + n] == '\x1b' ? 2 : 1),
                text.size() - at);
        }
        else {
            n = std::min<std::size_t>(2, text.size() - at);
        }
        return {n, 0};
    }
    if (b == '\n') {
        return {1, kNewlineCost};
    }
    if (b == '\r' || b == '\a' || b < 0x20) {
        return {1, 0};
    }
    const std::size_t n = b < 0x80    ? 1
                          : b >= 0xF0 ? 4
                          : b >= 0xE0 ? 3
                          : b >= 0xC0 ? 2
                                      : 1;
    return {std::min(n, text.size() - at), 1};
}

const char *kClearScreen = "\x1b[0m\x1b[H\x1b[2J";

std::vector<std::string> utf8Characters(const std::string &text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = (unsigned char)text[i];
        const std::size_t n = b < 0x80    ? 1
                              : b >= 0xF0 ? 4
                              : b >= 0xE0 ? 3
                              : b >= 0xC0 ? 2
                                          : 1;
        out.push_back(text.substr(i, n));
        i += n;
    }
    return out;
}
} // namespace

System::System(Config config)
    : config_(config)
    , shell_(world_)
    , canvas_(config.columns, config.rows, config.capabilities) {
    freshWorld();
}

void System::freshWorld() {
    World fresh;
    fresh.user = config_.user;
    fresh.hostname = config_.hostname;
    fresh.standalone = config_.standalone;
    fresh.columns = config_.columns;
    fresh.rows = config_.rows;
    fresh.caps = config_.capabilities;
    fresh.background = config_.background;
    fresh.soundAvailable = bool(config_.onBeep); // The host can play pips...
    fresh.sound =
        config_.sound && fresh.soundAvailable; // ...and starts on or off.
    fresh.slow =
        config_.revealCharsPerSecond > 0; // The host's choice to start with.
    fresh.rng = Rng(config_.seed);
    fresh.bootUnix = config_.unixTime;
    fresh.chat = config_.chat;
    fresh.now = world_.now;
    seedWorld(fresh);
    world_ = std::move(fresh);
    refreshUptimeFile();
    line_.clear();
    cursor_ = 0;
    historyIndex_ = 0;
}

Reaction System::take() {
    forwardBeeps();
    Reaction r;
    r.output = std::move(out_);
    out_.clear();
    r.exitCode = exitCode_;
    return r;
}

Reaction System::start() {
    started_ = true;
    if (config_.intro && findProgram("intro")) {
        launch("intro", {});
    }
    else {
        showWelcome();
    }
    return take();
}

void System::showWelcome() {
    if (clearOnWelcome_) {
        clearOnWelcome_ = false;
        out_ += kClearScreen;
    }
    const auto mark = out_.size();
    if (const auto *motd = world_.fs.find("/etc/motd")) {
        for (char c : motd->data) {
            if (c == '\n') {
                out_ += '\r';
            }
            out_ += c;
        }
    }
    showPrompt();
    const bool runStart = !config_.startCommand.empty() && !startCommandRan_;
    std::vector<std::string> command;
    if (runStart) {
        startCommandRan_ = true;
        for (const auto &character : utf8Characters(config_.startCommand)) {
            if (character.size() > 1 || (unsigned char)character[0] >= 0x20) {
                command.push_back(character);
            }
        }
    }
    if (!runStart || revealRate() > 0) {
        queueOutput(mark); // The welcome text appears at a pace too.
    }
    if (!runStart) {
        return;
    }
    if (revealRate() <= 0) {
        // No pace: typed and run at once.
        for (const auto &character : command) {
            insert(character);
        }
        submit();
        return;
    }
    // Typed for the visitor once the welcome text is out.
    autoType_ = std::move(command);
    autoTypeIndex_ = 0;
    autoTyping_ = true;
    autoStarted_ = false;
}

// The start command is typed one letter at a time (a pip each), with a pause
// before the first and before Enter, like a visitor who is not in a hurry.
void System::driveAutoType(Time now) {
    if (!autoTyping_ || !reveal_.empty() || program_) {
        return;
    }
    if (!autoStarted_) {
        autoStarted_ = true;
        autoTypeAt_ = now + Time(kAutoStartPauseMs);
        return;
    }
    if (now < autoTypeAt_) {
        return;
    }
    if (autoTypeIndex_ < autoType_.size()) {
        insert(autoType_[autoTypeIndex_++]);
        world_.beep(1100 + double((autoTypeIndex_ * 53) % 300), 15, 0.3);
        autoTypeAt_ =
            now + Time(autoTypeIndex_ == autoType_.size() ? kAutoEnterPauseMs
                                                          : kAutoTypeMs);
        return;
    }
    autoTyping_ = false;
    submit();
}

// A key: what is left is typed at once and the command runs.
void System::finishAutoType() {
    while (autoTypeIndex_ < autoType_.size()) {
        insert(autoType_[autoTypeIndex_++]);
    }
    autoTyping_ = false;
    submit();
}

std::string System::promptText(std::size_t *width) const {
    std::string where = world_.cwd;
    const auto home = world_.home();
    if (where == home) {
        where = "~";
    }
    else if (home != "/" &&
             where.compare(0, home.size() + 1, home + "/") == 0) {
        where = "~" + where.substr(home.size());
    }
    // A long path would leave no room for the line: keep the last two parts.
    if (where.size() >
        std::max<std::size_t>(12, std::size_t(world_.columns) / 3)) {
        const auto last = where.rfind('/');
        const auto before = last == std::string::npos || last == 0
                                ? std::string::npos
                                : where.rfind('/', last - 1);
        if (before != std::string::npos) {
            where = "..." + where.substr(before);
        }
    }
    const std::string plain =
        world_.user + "@" + world_.hostname + ":" + where + "$ ";
    if (width) {
        *width = plain.size();
    }
    return "\x1b[1;32m" + world_.user + "@" + world_.hostname +
           "\x1b[0m:\x1b[1;34m" + where + "\x1b[0m$ ";
}

void System::showPrompt() {
    line_.clear();
    cursor_ = 0;
    historyIndex_ = world_.history.size();
    tabCount_ = 0;
    out_ += promptText();
}

std::string System::lineText() const {
    std::string text;
    for (const auto &c : line_) {
        text += c;
    }
    return text;
}

void System::redrawLine() {
    out_ += "\r" + promptText() + lineText() + "\x1b[K";
    if (cursor_ < line_.size()) {
        out_ += "\x1b[" + std::to_string(line_.size() - cursor_) + "D";
    }
}

void System::insert(const std::string &character) {
    std::size_t promptWidth = 0;
    promptText(&promptWidth);
    const std::size_t limit = std::size_t(std::max<long long>(
        1, (long long)world_.columns - (long long)promptWidth - 1));
    if (line_.size() >= limit) {
        out_ += "\a";
        return;
    }
    line_.insert(line_.begin() + long(cursor_), character);
    ++cursor_;
    if (cursor_ == line_.size()) {
        out_ += character;
    }
    else {
        redrawLine();
    }
}

void System::eraseRange(std::size_t from, std::size_t to) {
    if (from >= to || to > line_.size()) {
        return;
    }
    line_.erase(line_.begin() + long(from), line_.begin() + long(to));
    cursor_ = from;
    redrawLine();
}

void System::history(int direction) {
    if (world_.history.empty()) {
        return;
    }
    if (direction < 0) {
        if (historyIndex_ == 0) {
            return;
        }
        if (historyIndex_ == world_.history.size()) {
            savedLine_ = lineText();
        }
        --historyIndex_;
    }
    else {
        if (historyIndex_ >= world_.history.size()) {
            return;
        }
        ++historyIndex_;
    }
    const auto text = historyIndex_ == world_.history.size()
                          ? savedLine_
                          : world_.history[historyIndex_];
    line_ = utf8Characters(text);
    std::size_t promptWidth = 0;
    promptText(&promptWidth);
    const std::size_t limit = std::size_t(std::max<long long>(
        1, (long long)world_.columns - (long long)promptWidth - 1));
    if (line_.size() > limit) {
        line_.resize(limit);
    }
    cursor_ = line_.size();
    redrawLine();
}

void System::complete() {
    std::string prefix;
    for (std::size_t i = 0; i < cursor_; ++i) {
        prefix += line_[i];
    }
    const auto result = shell_.complete(prefix);
    if (result.options.empty()) {
        out_ += "\a";
        return;
    }
    const auto word = prefix.substr(result.start);
    std::string replacement;
    if (result.options.size() == 1) {
        replacement = result.options[0];
        if (replacement.back() != '/') {
            replacement += ' ';
        }
    }
    else {
        replacement = result.options[0];
        for (const auto &o : result.options) {
            std::size_t n = 0;
            while (n < replacement.size() && n < o.size() &&
                   replacement[n] == o[n]) {
                ++n;
            }
            replacement.resize(n);
        }
    }
    if (replacement.size() > word.size()) {
        const auto chars = utf8Characters(word).size();
        line_.erase(line_.begin() + long(cursor_ - chars),
                    line_.begin() + long(cursor_));
        cursor_ -= chars;
        const auto added = utf8Characters(replacement);
        line_.insert(line_.begin() + long(cursor_), added.begin(), added.end());
        cursor_ += added.size();
        redrawLine();
        return;
    }
    if (result.options.size() > 1 && tabCount_ >= 2) {
        // Second Tab: list the choices below the line, then redraw it.
        std::size_t widest = 0;
        for (const auto &o : result.options) {
            widest = std::max(widest, o.size());
        }
        const std::size_t column = widest + 2;
        const std::size_t perRow =
            std::max<std::size_t>(1, std::size_t(world_.columns) / column);
        out_ += "\r\n";
        for (std::size_t i = 0; i < result.options.size(); ++i) {
            out_ += result.options[i] +
                    std::string(column - result.options[i].size(), ' ');
            if ((i + 1) % perRow == 0 || i + 1 == result.options.size()) {
                out_ += "\r\n";
            }
        }
        redrawLine();
    }
    else {
        out_ += "\a";
    }
}

// Words are runs of non-spaces, as for Ctrl-W.
void System::deleteWordBack() {
    std::size_t i = cursor_;
    while (i > 0 && line_[i - 1] == " ") {
        --i;
    }
    while (i > 0 && line_[i - 1] != " ") {
        --i;
    }
    eraseRange(i, cursor_);
}

void System::deleteWordForward() {
    std::size_t i = cursor_;
    while (i < line_.size() && line_[i] == " ") {
        ++i;
    }
    while (i < line_.size() && line_[i] != " ") {
        ++i;
    }
    eraseRange(cursor_, i);
}

void System::moveWord(int direction) {
    std::size_t i = cursor_;
    if (direction < 0) {
        while (i > 0 && line_[i - 1] == " ") {
            --i;
        }
        while (i > 0 && line_[i - 1] != " ") {
            --i;
        }
        if (i < cursor_) {
            out_ += "\x1b[" + std::to_string(cursor_ - i) + "D";
        }
    }
    else {
        while (i < line_.size() && line_[i] == " ") {
            ++i;
        }
        while (i < line_.size() && line_[i] != " ") {
            ++i;
        }
        if (i > cursor_) {
            out_ += "\x1b[" + std::to_string(i - cursor_) + "C";
        }
    }
    cursor_ = i;
}

void System::handleShellKey(const Key &key) {
    using T = Key::Type;
    if (!key.is(T::Tab)) {
        tabCount_ = 0;
    }
    switch (key.type) {
    case T::Text:
        if (key.alt && key.text == "d") { // Alt+D: a word forward.
            deleteWordForward();
        }
        else if (key.alt && key.text == "b") {
            moveWord(-1);
        }
        else if (key.alt && key.text == "f") {
            moveWord(1);
        }
        else if (key.text == "\n") {
            submit();
        }
        else if (!key.text.empty() && (unsigned char)key.text[0] >= 0x20) {
            insert(key.text);
        }
        break;
    case T::Enter:
        submit();
        break;
    case T::Backspace:
        if (key.alt || key.ctrl) { // Alt+Backspace: a word back.
            deleteWordBack();
        }
        else if (cursor_ > 0) {
            if (cursor_ == line_.size()) {
                line_.pop_back();
                --cursor_;
                out_ += "\b \b";
            }
            else {
                eraseRange(cursor_ - 1, cursor_);
            }
        }
        break;
    case T::Delete:
        if (key.alt || key.ctrl) { // Ctrl+Delete: a word forward.
            deleteWordForward();
        }
        else {
            eraseRange(cursor_, std::min(line_.size(), cursor_ + 1));
        }
        break;
    case T::Left:
        if (key.alt || key.ctrl) {
            moveWord(-1);
        }
        else if (cursor_ > 0) {
            --cursor_;
            out_ += "\x1b[D";
        }
        break;
    case T::Right:
        if (key.alt || key.ctrl) {
            moveWord(1);
        }
        else if (cursor_ < line_.size()) {
            ++cursor_;
            out_ += "\x1b[C";
        }
        break;
    case T::Home:
        if (cursor_ > 0) {
            out_ += "\x1b[" + std::to_string(cursor_) + "D";
            cursor_ = 0;
        }
        break;
    case T::End:
        if (cursor_ < line_.size()) {
            out_ += "\x1b[" + std::to_string(line_.size() - cursor_) + "C";
            cursor_ = line_.size();
        }
        break;
    case T::Up:
        history(-1);
        break;
    case T::Down:
        history(1);
        break;
    case T::Tab:
        ++tabCount_;
        complete();
        break;
    case T::Ctrl: {
        const char c = key.text.empty() ? char(0) : key.text[0];
        if (c == 'c') {
            out_ += "^C\r\n";
            showPrompt();
        }
        else if (c == 'd') {
            if (line_.empty()) {
                out_ += "logout\r\n";
                exitCode_ = 0;
            }
            else {
                eraseRange(cursor_, std::min(line_.size(), cursor_ + 1));
            }
        }
        else if (c == 'l') {
            out_ += "\x1b[H\x1b[2J";
            redrawLine();
        }
        else if (c == 'u') {
            eraseRange(0, cursor_);
        }
        else if (c == 'k') {
            eraseRange(cursor_, line_.size());
        }
        else if (c == 'w') {
            deleteWordBack();
        }
        else if (c == 'a') {
            handleShellKey(Key{Key::Type::Home});
        }
        else if (c == 'e') {
            handleShellKey(Key{Key::Type::End});
        }
        break;
    }
    default:
        break; // Escape, paging and mouse reports mean nothing to a prompt.
    }
}

void System::submit() {
    const std::string line = lineText();
    out_ += "\r\n";
    line_.clear();
    cursor_ = 0;
    if (line.find_first_not_of(" \t") == std::string::npos) {
        showPrompt();
        return;
    }
    if (world_.history.empty() || world_.history.back() != line) {
        world_.history.push_back(line);
    }
    const auto mark = out_.size();
    const auto result = shell_.run(line);
    out_ += result;
    const bool restores = world_.resetRequest;
    afterCommand();
    if (!result.empty() || restores) { // A bare prompt is never held back.
        queueOutput(mark);
    }
}

// Pips collected from the reveal and from programs go to the host's listener.
// /proc/uptime follows the clock, once a second.
void System::refreshUptimeFile() {
    const auto seconds = world_.uptimeSeconds();
    if (seconds == procUptimeAt_) {
        return;
    }
    procUptimeAt_ = seconds;
    world_.fs.put("/proc/uptime",
                  std::to_string(seconds) + ".00 " +
                      std::to_string(seconds * hardware::kCpuCores * 97 / 100) +
                      ".00\n",
                  "root",
                  0444,
                  world_.unixNow());
}

// Characters per second while text is revealed; 0 when the typing speed is off
// (`slow off`). With a host that started without a pace, `slow on` uses a
// default.
int System::revealRate() const {
    if (!world_.slow) {
        return 0;
    }
    if (config_.revealCharsPerSecond > 0) {
        return config_.revealCharsPerSecond;
    }
    return world_.slowAsked ? 1250 : 0; // Only if `slow on` was typed.
}

void System::forwardBeeps() {
    if (!world_.sound || !config_.onBeep) {
        world_.beeps.clear();
        return;
    }
    for (auto beep : world_.beeps) {
        beep.at = now_;
        config_.onBeep(beep);
    }
    world_.beeps.clear();
}

// ---- Revealing program output at a pace -----------------------------------

void System::queueOutput(std::size_t mark) {
    if (revealRate() <= 0 || mark >= out_.size()) {
        return;
    }
    double cost = 0;
    for (std::size_t at = mark; at < out_.size();) {
        const auto unit = nextUnit(out_, at);
        cost += unit.cost;
        at += unit.bytes;
    }
    if (cost <= 0) {
        return; // Nothing visible (only escape sequences).
    }
    reveal_ = out_.substr(mark);
    out_.resize(mark);
    const double seconds = std::clamp(
        cost / double(revealRate()), kLeastRevealSeconds, kMostRevealSeconds);
    revealRate_ = cost / seconds;
    revealCredit_ = 0;
    revealAt_ = now_;
    revealAnchored_ = false;
    if (exitCode_) { // `exit` happens after its text has been shown.
        deferredExit_ = exitCode_;
        exitCode_.reset();
    }
}

void System::drainReveal(Time now) {
    if (!revealAnchored_) {
        // The pace starts at the first tick after the text was queued, so the
        // time before it (a host that ticks late) is not paid out as a burst.
        revealAnchored_ = true;
        revealAt_ = now;
        return;
    }
    const double seconds =
        std::clamp(double((now - revealAt_).count()), 0.0, 100.0) / 1000.0;
    revealAt_ = now;
    revealCredit_ += revealRate_ * seconds;
    std::size_t used = 0;
    unsigned char lastShown = 0;
    bool lineDone = false;
    while (used < reveal_.size()) {
        const auto unit = nextUnit(reveal_, used);
        if (unit.cost > revealCredit_) {
            break;
        }
        revealCredit_ -= unit.cost;
        if (unit.cost > 0) {
            lineDone = reveal_[used] ==
                       '\n'; // A pip for a line only when it ends one.
            lastShown = (unsigned char)reveal_[used];
        }
        used += unit.bytes;
    }
    if (lastShown && (now - lastBeepAt_).count() >= kBeepGapMs) {
        lastBeepAt_ = now;
        if (lineDone) {
            world_.beep(520, 50, 0.35);
        }
        else {
            // The pitch follows the character, so the pips make a pattern.
            world_.beep(1000 + double((lastShown * 37) % 380), 18, 0.25);
        }
    }
    out_.append(reveal_, 0, used);
    reveal_.erase(0, used);
    if (reveal_.empty()) {
        revealCredit_ = 0;
        if (deferredExit_) {
            exitCode_ = deferredExit_;
            deferredExit_.reset();
        }
    }
}

void System::flushReveal() {
    out_ += reveal_;
    reveal_.clear();
    revealCredit_ = 0;
    if (deferredExit_) {
        exitCode_ = deferredExit_;
        deferredExit_.reset();
    }
}

// One key answers: y crashes the machine, anything else backs out.
void System::answerQuestion(const Key &key) {
    answering_ = false;
    const bool yes = key.isText("y") || key.isText("Y");
    out_ += (yes ? "y" : "n");
    out_ += "\r\n";
    if (question_ == Question::Chat) {
        world_.chatQuestion = false;
        if (yes && world_.chat.available()) {
            launch("chat", {});
            if (program_) {
                return;
            }
        }
        else {
            out_ += "Not connecting.\r\n";
        }
        showPrompt();
        return;
    }
    world_.crashQuestion.clear();
    if (yes) {
        startCrash();
        return;
    }
    out_ += "Wise choice.\r\n";
    showPrompt();
}

// The disk is gone and the machine hangs on a boot failure; it restarts by
// itself (the "crash" program).
void System::startCrash() {
    // Everything under / goes.
    for (const auto &entry : world_.fs.list("/")) {
        world_.fs.remove(VirtualFs::join("/", entry.first), "root", true);
    }
    world_.history.clear();
    launch("crash", {});
}

void System::afterCommand() {
    if (world_.chatQuestion) {
        question_ = Question::Chat;
        answering_ = true;
        return;
    }
    if (!world_.crashQuestion.empty()) {
        question_ = Question::Crash;
        answering_ = true; // No prompt: it waits for the answer.
        return;
    }
    if (world_.rebootRequest) {
        // The system had enough: a new world, and the intro again.
        world_.rebootRequest = false;
        out_ += "\r\nRebooting the system...\r\n";
        freshWorld();
        clearOnWelcome_ = true;
        if (findProgram("intro")) {
            launch("intro", {});
            return;
        }
        showWelcome();
        return;
    }
    if (world_.resetRequest) {
        world_.resetRequest = false;
        freshWorld();
        out_ += kClearScreen;
        out_ += "The system was restored to its first state.\r\n";
    }
    if (world_.exitRequest) {
        exitCode_ = *world_.exitRequest;
        world_.exitRequest.reset();
        out_ += "";
        return;
    }
    if (world_.launch) {
        const auto request = *world_.launch;
        world_.launch.reset();
        launch(request.name, request.args);
        if (program_) {
            return;
        }
    }
    showPrompt();
}

void System::launch(const std::string &name,
                    const std::vector<std::string> &args) {
    const auto *info = findProgram(name);
    if (!info) {
        out_ += "matterm: " + name + ": program not available\r\n";
        return;
    }
    program_ = info->make(args);
    ++programSerial_;
    programName_ = name;
    programBegun_ = false;
    nextFrame_ = now_;
    if (program_->inlineMode()) {
        return; // It draws on the normal screen by itself.
    }
    canvas_.resize(world_.columns, world_.rows);
    canvas_.setCapabilities(world_.caps);
    out_ += world_.caps.alternateScreen ? kEnterApp : kClearScreen;
    out_ += "\x1b[?25l";
    if (program_->wantsMouse()) {
        out_ += "\x1b[?1000h\x1b[?1006h";
    }
}

void System::endProgram() {
    const bool wasIntro = programName_ == "intro";
    if (program_->inlineMode()) {
        out_ += program_->leave(world_);
    }
    else {
        out_ += world_.caps.alternateScreen
                    ? kLeaveApp
                    : (std::string(kClearScreen) + "\x1b[?25h");
    }
    program_->end(world_);
    for (char c : program_->farewell()) {
        if (c == '\n') {
            out_ += '\r';
        }
        out_ += c;
    }
    program_.reset();
    for (char c : pendingText_) {
        if (c == '\n') {
            out_ += '\r';
        }
        out_ += c;
    }
    pendingText_.clear();
    if (world_.rebootRequest) { // The crash is over: a new world and the intro.
        world_.rebootRequest = false;
        freshWorld();
        clearOnWelcome_ = true;
        if (findProgram("intro")) {
            launch("intro", {});
            return;
        }
        showWelcome();
        return;
    }
    if (world_.launch) { // A program (or a click egg) asked for the next one.
        const auto request = *world_.launch;
        world_.launch.reset();
        world_.programInput = request.input;
        launch(request.name, request.args);
        if (program_) {
            return;
        }
    }
    if (wasIntro) {
        showWelcome();
    }
    else {
        showPrompt();
    }
}

// A left click while a program runs: if the word drawn there is a click egg,
// the program ends and the egg takes effect.
void System::clicked(const Key &key) {
    const auto word = canvas_.wordAt(key.column, key.row);
    if (word.empty()) {
        return;
    }
    if (const auto *egg = matchClickEgg(world_, programName_, word)) {
        std::string text;
        if (applyEgg(world_, *egg, text)) {
            pendingText_ += text;
            endProgram();
        }
    }
}

Reaction System::input(std::string_view bytes) {
    for (const auto &key : decoder_.feed(bytes)) {
        if (exitCode_) {
            break;
        }
        // Space, Enter, Escape and Ctrl-C only skip; any other key is then
        // typed, like type-ahead. Mouse reports mean nothing while text is
        // revealed or the start command is typed.
        const bool skipKey = key.is(Key::Type::Enter) ||
                             key.is(Key::Type::Escape) || key.isText(" ") ||
                             (key.is(Key::Type::Ctrl) && key.text == "c");
        if (!reveal_.empty()) {
            // A key shows the rest at once.
            if (key.is(Key::Type::Mouse)) {
                continue;
            }
            flushReveal();
            if (skipKey) {
                continue;
            }
            if (exitCode_) {
                break;
            }
        }
        if (autoTyping_) {
            // A key makes the start command run now, and its output show.
            if (key.is(Key::Type::Mouse)) {
                continue;
            }
            finishAutoType();
            flushReveal();
            if (skipKey) {
                continue;
            }
            if (exitCode_) {
                break;
            }
        }
        if (answering_) {
            if (!key.is(Key::Type::Mouse)) {
                answerQuestion(key);
            }
            continue;
        }
        if (program_) {
            const auto serial = programSerial_;
            if (key.is(Key::Type::Mouse) && key.press && !key.motion &&
                key.button == 0) {
                clicked(key);
            }
            // The click may have ended the program (and started the next one,
            // which has not seen this key).
            if (program_ && serial == programSerial_) {
                program_->key(world_, key);
                nextFrame_ = now_; // Show the effect of the key at once.
                if (program_->inlineMode() && programBegun_) {
                    out_ += program_->rawFrame(world_, now_);
                }
                if (program_->finished()) {
                    endProgram();
                }
            }
        }
        else {
            handleShellKey(key);
        }
    }
    return take();
}

Reaction System::tick(Time now) {
    if (!base_.count() && !now_.count()) {
        base_ = now;
    }
    now_ = now;
    world_.now = now - base_;
    refreshUptimeFile();
    if (!reveal_.empty()) {
        // A program that is waiting to start shows nothing until the text is
        // out.
        drainReveal(now);
        return take();
    }
    if (!program_ && !exitCode_) {
        driveAutoType(now);
    }
    if (program_ && !exitCode_) {
        if (!programBegun_) {
            programBegun_ = true;
            program_->begin(world_, now);
            nextFrame_ = now;
        }
        if (now >= nextFrame_) {
            if (program_->inlineMode()) {
                out_ += program_->rawFrame(world_, now);
            }
            else {
                program_->frame(world_, now, canvas_);
                out_ += canvas_.flush();
            }
            nextFrame_ = now + program_->interval();
            if (program_->finished()) {
                endProgram();
            }
        }
    }
    return take();
}

void System::resize(int columns, int rows) {
    world_.columns = std::max(1, columns);
    world_.rows = std::max(1, rows);
    if (program_ && program_->inlineMode()) {
        program_->resize(world_, world_.columns, world_.rows);
        if (programBegun_) {
            out_ += program_->rawFrame(world_, now_);
        }
    }
    else if (program_) {
        canvas_.resize(world_.columns, world_.rows);
        program_->resize(world_, world_.columns, world_.rows);
        out_ += "\x1b[0m\x1b[2J";
    }
}

Reaction System::external(const std::string &kind, std::string_view data) {
    if (program_ && !exitCode_ && programBegun_) {
        program_->external(world_, kind, data, now_);
        if (program_->inlineMode()) {
            out_ += program_->rawFrame(world_, now_);
        }
        if (program_->finished()) {
            endProgram();
        }
    }
    return take();
}

Reaction System::chatOpened() {
    return external("opened", {});
}

Reaction System::chatLine(std::string_view line) {
    return external("line", line);
}

Reaction System::chatClosed() {
    return external("closed", {});
}

std::optional<Time> System::nextDeadline() const {
    if (!reveal_.empty()) {
        return revealAt_ + Time(16); // The next little piece of text.
    }
    if (autoTyping_ && !program_) {
        return autoStarted_ ? autoTypeAt_ : now_;
    }
    if (program_ && !exitCode_) {
        return nextFrame_;
    }
    return std::nullopt;
}

} // namespace fakeos
