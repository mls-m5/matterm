#include "ptysessionhost.h"
#include <algorithm>
#include <array>
#include <csignal>

namespace server {
PtySessionHost::PtySessionHost(ProcessConfig config, Send send, Close close)
    : config_(std::move(config))
    , send_(std::move(send))
    , close_(std::move(close)) {}
void PtySessionHost::send(std::string bytes) {
    send_(std::move(bytes));
}
void PtySessionHost::closeTransport(int code) {
    close_(code);
}
bool PtySessionHost::startProcess(std::uint16_t columns, std::uint16_t rows) {
    if (process_)
        return false;
    try {
        process_ =
            std::make_unique<PtyProcess>(config_.arguments,
                                         columns,
                                         rows,
                                         "xterm-256color",
                                         PtyProcess::ColorCapability::TrueColor,
                                         config_.environmentAllowList,
                                         config_.workingDirectory,
                                         config_.extraEnvironment);
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}
bool PtySessionHost::writeInput(std::string_view bytes) {
    constexpr std::size_t limit = 1024 * 1024;
    if (!process_ || bytes.size() > limit - input_.size())
        return false;
    input_.append(bytes);
    return true;
}
void PtySessionHost::resizeProcess(std::uint16_t columns,
                                   std::uint16_t rows,
                                   std::uint16_t pixelWidth,
                                   std::uint16_t pixelHeight) {
    if (process_)
        process_->resize(columns, rows, pixelWidth, pixelHeight);
}
namespace {
protocol::Time steadyNow() {
    return std::chrono::duration_cast<protocol::Time>(
        std::chrono::steady_clock::now().time_since_epoch());
}
} // namespace
PtySessionHost::~PtySessionHost() {
    // Whatever the path here, never leave the group running. The leader is
    // still unreaped unless finished_ (then the group was already killed).
    if (process_ && !killed_) {
        process_->signalProcessGroup(SIGKILL);
    }
}
void PtySessionHost::beginCleanup(protocol::Time now) {
    if (killAt_ || !process_) {
        return;
    }
    process_->signalProcessGroup(SIGHUP);
    killAt_ = now + config_.terminationGrace;
}
void PtySessionHost::terminateProcess() {
    if (!process_ || drainBy_) {
        return;
    }
    const auto now = steadyNow();
    beginCleanup(now);
    drainBy_ = now + config_.terminationDrainTimeout;
}
void PtySessionHost::pump(protocol::ServerSession &session,
                          protocol::Time now) {
    pump(session.outputBudget(),
         now,
         {[&](std::string_view bytes) { session.onProcessOutput(bytes, now); },
          [&](int code, int signal, bool drained) {
              if (drained)
                  session.onProcessExit(code, signal, now);
              else
                  close_(1000);
          }});
}
void PtySessionHost::pump(std::size_t outputBudget,
                          protocol::Time now,
                          const ProcessEvents &events) {
    if (!process_ || finished_) {
        return;
    }
    const bool terminating = drainBy_.has_value();
    // The leader ending, however it ended, ends the whole session's process
    // group: HUP now, KILL after the grace period.
    if (!killAt_ && process_->peekExit()) {
        beginCleanup(now);
    }
    if (killAt_ && !killed_ && now >= *killAt_) {
        process_->signalProcessGroup(SIGKILL);
        killed_ = true;
    }
    if (!terminating && !input_.empty()) {
        input_.erase(0, process_->write(input_));
    }
    std::array<char, 16375> buffer{};
    // One bounded read per pump, even when output is continually available.
    const auto budget = std::min(buffer.size(), outputBudget);
    if (budget && !process_->closed()) {
        const auto read = process_->read(buffer.data(), budget);
        if (read.count)
            events.output(std::string_view(buffer.data(), read.count));
    }
    if (!killed_) {
        return; // Keep the leader unreaped until the group is gone.
    }
    const bool drained = process_->closed();
    const bool gaveUp = drainBy_ && now >= *drainBy_;
    if (!drained && !gaveUp) {
        return;
    }
    const auto status = process_->pollExit();
    if (!status) {
        return; // SIGKILLed leader not yet reapable; try again next pump.
    }
    finished_ = true;
    input_.clear();
    events.exited(status->code, status->signal, drained);
}
} // namespace server
