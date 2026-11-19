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
                                         config_.environmentAllowList);
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
void PtySessionHost::terminateProcess() {
    if (process_ && !killAt_) {
        process_->signalProcessGroup(SIGHUP);
        killAt_ = std::chrono::duration_cast<protocol::Time>(
                      std::chrono::steady_clock::now().time_since_epoch()) +
                  config_.terminationGrace;
    }
}
void PtySessionHost::pump(protocol::ServerSession &session,
                          protocol::Time now) {
    if (!process_ || exitReported_)
        return;
    if (killAt_ && now >= *killAt_)
        process_->signalProcessGroup(SIGKILL);
    if (!input_.empty())
        input_.erase(0, process_->write(input_));
    std::array<char, 16375> buffer{};
    // One bounded read per pump, even when output is continually available.
    const auto budget = std::min(buffer.size(), session.outputBudget());
    if (budget && !process_->closed()) {
        const auto read = process_->read(buffer.data(), budget);
        if (read.count)
            session.onProcessOutput(std::string_view(buffer.data(), read.count),
                                    now);
    }
    if (process_->closed()) {
        const auto status = process_->pollExit();
        if (status) {
            exitReported_ = true;
            input_.clear();
            session.onProcessExit(status->code, status->signal, now);
        }
    }
}
} // namespace server
