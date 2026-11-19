#pragma once

#include "ptyprocess.h"
#include "terminalmodes.h"
#include "vtevents.h"

#include <functional>
#include <memory>

class TerminalModel;

// Native transport bridge. Portable model/parser/input encoders remain
// separate. Construct before SDL/worker threads; pump() uses nonblocking
// PTY I/O and byte budgets. Callbacks run synchronously.
class PtySession {
public:
    PtySession(const std::vector<std::string> &arguments,
               int columns,
               int rows,
               VtEvents events = {},
               const std::string &term = "vt100",
               PtyProcess::ColorCapability color =
                   PtyProcess::ColorCapability::Unspecified);
    ~PtySession();
    PtySession(const PtySession &) = delete;
    PtySession &operator=(const PtySession &) = delete;
    PtySession(PtySession &&) = delete;
    PtySession &operator=(PtySession &&) = delete;

    TerminalModel &model() noexcept;
    const TerminalModes &modes() const noexcept;
    // False means closed or queue limit exceeded; at most 1 MiB is pending.
    bool send(std::string_view bytes);
    // Raw incoming bytes, before parsing. The view is valid only during the
    // callback. Do not reenter the session. Empty observer disables capture.
    void setOutputObserver(std::function<void(std::string_view)> observer);
    void pump(std::size_t readBudget = 65536, std::size_t writeBudget = 65536);
    void resize(int columns, int rows);
    bool finished() const noexcept;
    std::optional<PtyProcess::ExitStatus> exitStatus() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
