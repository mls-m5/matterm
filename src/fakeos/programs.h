#pragma once

#include "canvas.h"
#include "keys.h"
#include "world.h"

#include <memory>
#include <string>
#include <vector>

namespace fakeos {

// A full-screen program: it owns the terminal (alternate screen) until it
// finishes, draws into an AsciiCanvas, and may read or change the World, so a
// discovery made in a program is still there at the shell prompt.
class Program {
public:
    virtual ~Program() = default;
    // Called once, at the first frame time.
    virtual void begin(World &, Time) {}
    // Keys (and mouse reports) reach the program; it decides when to finish.
    virtual void key(World &, const Key &) {}
    virtual void resize(World &, int /*columns*/, int /*rows*/) {}
    // Advance to `now` and draw the frame.
    virtual void frame(World &world, Time now, AsciiCanvas &canvas) = 0;
    virtual bool finished() const = 0;
    // Called after the screen was restored, to change the world.
    virtual void end(World &) {}
    virtual Time interval() const {
        return Time(33);
    }
    // Whether the program wants mouse reports (DECSET 1000/1006).
    virtual bool wantsMouse() const {
        return false;
    }
    // An inline program draws on the normal screen with its own escape
    // sequences (no alternate screen, no canvas): rawFrame() returns what to
    // write now, leave() what to write when it ends. The chat is one.
    virtual bool inlineMode() const {
        return false;
    }
    virtual std::string rawFrame(World &, Time) {
        return {};
    }
    virtual std::string leave(World &) {
        return {};
    }
    // Something from the host (the chat connection): "opened", "line",
    // "closed".
    virtual void external(World &,
                          const std::string &,
                          std::string_view,
                          Time) {}
    // Printed after the program ended and the screen was restored.
    virtual std::string farewell() const {
        return {};
    }
};

using ProgramFactory =
    std::unique_ptr<Program> (*)(const std::vector<std::string> &args);

struct ProgramInfo {
    const char *name;
    ProgramFactory make;
    const char *summary;
};

const std::vector<ProgramInfo> &programs();
const ProgramInfo *findProgram(const std::string &name);

std::unique_ptr<Program> makeFire(const std::vector<std::string> &args);
std::unique_ptr<Program> makeGradient(const std::vector<std::string> &args);
std::unique_ptr<Program> makeIntro(const std::vector<std::string> &args);
std::unique_ptr<Program> makePager(const std::vector<std::string> &args);
std::unique_ptr<Program> makeTop(const std::vector<std::string> &args);
std::unique_ptr<Program> makeCrumble(const std::vector<std::string> &args);
std::unique_ptr<Program> makeMatrix(const std::vector<std::string> &args);
std::unique_ptr<Program> makeCrash(const std::vector<std::string> &args);
std::unique_ptr<Program> makeChat(const std::vector<std::string> &args);
std::unique_ptr<Program> makeClock(const std::vector<std::string> &args);
std::unique_ptr<Program> makeStarfield(const std::vector<std::string> &args);
std::unique_ptr<Program> makeAquarium(const std::vector<std::string> &args);
std::unique_ptr<Program> makeSnow(const std::vector<std::string> &args);
std::unique_ptr<Program> makeLife(const std::vector<std::string> &args);
std::unique_ptr<Program> makeTrain(const std::vector<std::string> &args);

} // namespace fakeos
