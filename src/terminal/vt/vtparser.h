#pragma once

#include "colorreference.h"
#include "rgb8.h"
#include "terminalmodes.h"
#include "vtevents.h"

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
    // follow.
    void finish();
    // Discard pending input without replacement, retaining completed model
    // state.
    void resetInput() noexcept;
    const TerminalModes &modes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
