#pragma once

#include "ptyprocess.h"
#include "terminallink.h"

#include <functional>
#include <optional>
#include <string>

// TerminalLink for a local Linux PTY: the native counterpart of the remote
// ClientSession, so a front end can use either through the same interface.
// Nonblocking: call pump() from the front end's loop; callbacks run inside
// pump() (and `exited` only after the PTY is drained and the child reaped).
// Like PtyProcess, construct before graphics or worker threads exist.
class PtyLink final : public TerminalLink {
public:
    PtyLink(const std::vector<std::string> &arguments,
            int columns,
            int rows,
            const std::string &term = "xterm-256color",
            PtyProcess::ColorCapability color =
                PtyProcess::ColorCapability::Unspecified);

    void setEvents(Events events) override;
    // Queues bytes for the child (at most 1 MiB pending). False when the queue
    // is full or the PTY is closed.
    bool sendInput(std::string_view bytes) override;
    // The PTY has no pixel size; pixelWidth/pixelHeight are ignored.
    void resize(int columns,
                int rows,
                int pixelWidth = 0,
                int pixelHeight = 0) override;
    // Hangs up the child; `exited` follows from pump() when it has ended.
    void close() override;

    // Optional: called once when the PTY reports end of output, which can be
    // before the exit status is known.
    void setEndOfOutputHandler(std::function<void()> handler);
    void pump(std::size_t readBudget = 65536, std::size_t writeBudget = 65536);
    // True once all output was delivered and the exit status is known.
    bool finished() const noexcept;
    std::optional<PtyProcess::ExitStatus> exitStatus() const noexcept;

private:
    PtyProcess process_;
    Events events_;
    std::function<void()> endOfOutput_;
    std::string pending_;
    std::size_t offset_ = 0;
    bool eof_ = false;
    bool exitedReported_ = false;
    std::optional<PtyProcess::ExitStatus> status_;
};
