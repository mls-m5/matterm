#pragma once

#include "canvas.h"
#include "keys.h"
#include "programs.h"
#include "shell.h"
#include "types.h"
#include "world.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fakeos {

struct Config {
    int columns = 80;
    int rows = 24;
    std::uint64_t seed = 1; // For the animations; the same seed repeats them.
    std::string user = "guest";
    std::string hostname = "matterm";
    Capabilities capabilities;
    bool intro = true;
    // The terminal's default background color (the matterm palette's is
    // 30,30,30).
    Rgb background{30, 30, 30};
    // Program output (commands, help, the motd) appears at about this many
    // characters per second, a new line costing a few more; any key shows the
    // rest at once. Typing and prompts are never delayed. 0: all at once.
    int revealCharsPerSecond = 0;
    // Pips while text is revealed or typed (see Beep). The core produces events
    // only when `sound` is true and someone listens; the host plays them.
    bool sound = false;
    std::function<void(const Beep &)> onBeep;
    // Typed and run at the first prompt, as if the visitor had entered it (for
    // a demo that starts with something to look at). With a reveal pace it is
    // typed letter by letter (with pips) after the intro and the welcome text,
    // and a key makes it run at once; without one it runs immediately. Empty:
    // none.
    std::string startCommand;
    // The pretend clock starts here (Unix seconds); the default is a fixed
    // date so that output is reproducible.
    std::int64_t unixTime = 1767225600; // 2026-01-01 00:00:00 UTC
};

// The pretend system: bytes in, bytes out. The host feeds it what the user
// typed (input), the terminal size (resize) and the time (tick), and writes
// what it returns. It does no I/O of its own.
//
// It first plays the intro (unless disabled), then shows a shell prompt in a
// small virtual Unix. Full-screen programs found on disk take over the screen
// and may change the world they run in.
class System {
public:
    explicit System(Config config);

    Reaction start();
    Reaction input(std::string_view bytes);
    Reaction tick(Time now);
    void resize(int columns, int rows);
    // When tick() next has something to do; empty when only input can wake it.
    std::optional<Time> nextDeadline() const;

    const World &world() const {
        return world_;
    }
    bool inProgram() const {
        return program_ != nullptr;
    }
    // Program output is still being revealed (Config::revealCharsPerSecond).
    bool revealing() const {
        return !reveal_.empty();
    }

private:
    void freshWorld();
    void showWelcome();
    void showPrompt();
    std::string promptText(std::size_t *width = nullptr) const;
    void redrawLine();
    void handleShellKey(const Key &key);
    void insert(const std::string &character);
    void eraseRange(std::size_t from, std::size_t to);
    void complete();
    void history(int direction);
    void submit();
    void afterCommand();
    void launch(const std::string &name, const std::vector<std::string> &args);
    void clicked(const Key &key);
    void endProgram();
    // Output produced since `mark` (an offset into out_) is held back and shown
    // at the configured pace, when it is more than a line or two.
    void queueOutput(std::size_t mark);
    void drainReveal(Time now);
    void flushReveal();
    void forwardBeeps();
    void answerQuestion(const Key &key);
    void startCrash();
    void refreshUptimeFile();
    int revealRate() const;
    void driveAutoType(Time now);
    void finishAutoType();
    std::string lineText() const;
    Reaction take();

    Config config_;
    World world_;
    Shell shell_;
    KeyDecoder decoder_;
    AsciiCanvas canvas_;
    std::unique_ptr<Program> program_;
    std::string programName_; // The builtin that runs, not the file's name.
    std::string pendingText_; // Shown after the program ends (click eggs).
    std::uint64_t programSerial_ = 0; // Changes whenever a program starts.
    bool programBegun_ = false;
    Time now_{0}, base_{0}, nextFrame_{0};
    bool started_ = false;
    std::string out_;
    std::optional<int> exitCode_;

    std::vector<std::string> line_; // One entry per code point.
    std::size_t cursor_ = 0;
    std::size_t historyIndex_ = 0;
    std::string savedLine_;
    int tabCount_ = 0;
    std::string reveal_;      // Output waiting to be shown.
    double revealCredit_ = 0; // Units that may still be shown.
    double revealRate_ = 0;   // Units per second for the current text.
    Time revealAt_{0};
    bool revealAnchored_ =
        false; // The first tick after queueing sets revealAt_.
    Time lastBeepAt_{-1000};
    std::optional<int> deferredExit_; // `exit` after the text has been shown.
    bool startCommandRan_ = false;
    bool clearOnWelcome_ = false; // After a reboot the old screen goes first.
    std::int64_t procUptimeAt_ = -1;
    // The start command being typed for the visitor.
    std::vector<std::string> autoType_;
    std::size_t autoTypeIndex_ = 0;
    Time autoTypeAt_{0};
    bool autoTyping_ = false;
    bool autoStarted_ = false;
    bool answering_ = false; // A question is waiting for y or n.
};

} // namespace fakeos
