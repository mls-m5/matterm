#pragma once

#include "colorreference.h"
#include "rgb8.h"
#include "terminalmodes.h"
#include "vtevents.h"

#include <functional>
#include <memory>
#include <string_view>

class TerminalModel;

// Incremental UTF-8/VT subset. Borrows the model, which must outlive the
// parser. Owns protocol rendition state: do not independently change model
// colors/modes while feeding this parser. Resizing and viewport navigation
// remain independent. No SDL, PTY, clipboard, file access, or process
// execution.
class VtParser {
public:
    explicit VtParser(TerminalModel &model, VtEvents events = {});
    // Model-free, opt-in decoded output fragments. Shares this parser's VT
    // state machine, but executes NO terminal operations (including REP),
    // replies, titles or palette actions, and retains no string payloads.
    // text receives ground Unicode plus CR/LF/HT boundaries, not screen rows;
    // views expire after the callback. Callbacks must not re-enter the parser.
    struct TextOnly {
        std::function<void(std::u32string_view)> text;
        // True ONLY at stream offset 0 or a proven lexical ground boundary.
        // Cold tails must use false: discard until ST (ESC backslash / UTF-8
        // C1 ST) or CAN/SUB, never synchronize at BEL or LF alone.
        bool startAtKnownBoundary = true;
    };
    explicit VtParser(TextOnly options);
    // False for a normal model parser. Invalid UTF-8 in text-only mode fails
    // closed until a synchronization marker (literal U+FFFD does so too).
    bool textSynchronized() const noexcept;

    // Compatibility override: these defaults are explicit RGB, not theme
    // defaults.
    VtParser(TerminalModel &model,
             VtEvents events,
             Rgb8 foreground,
             Rgb8 background);
    ~VtParser();
    VtParser(const VtParser &) = delete;
    VtParser &operator=(const VtParser &) = delete;
    VtParser(VtParser &&) = delete;
    VtParser &operator=(VtParser &&) = delete;

    void feed(std::string_view bytes);
    // End of stream: flush truncated UTF-8 as U+FFFD, discard incomplete
    // controls. Completed model state/rendition is retained. More input may
    // follow. Text-only mode flushes pending UTF-8 but preserves lexical
    // discard state, so finish() cannot expose an unfinished control payload.
    void finish();
    // Discard pending input without replacement, retaining completed model
    // state. In text-only mode this loses synchronization and waits for ST or
    // CAN/SUB; it never assumes a new arbitrary byte offset is ground.
    void resetInput() noexcept;
    const TerminalModes &modes() const noexcept;

private:
    struct Impl;
    struct TextImpl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<TextImpl> textImpl_;
};
