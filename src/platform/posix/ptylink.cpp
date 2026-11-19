#include "ptylink.h"

#include <algorithm>
#include <array>

namespace {
constexpr std::size_t maxPending = 1024 * 1024;
}

PtyLink::PtyLink(const std::vector<std::string> &arguments,
                 int columns,
                 int rows,
                 const std::string &term,
                 PtyProcess::ColorCapability color)
    : process_(arguments, columns, rows, term, color) {}

void PtyLink::setEvents(Events events) {
    events_ = std::move(events);
}
void PtyLink::setEndOfOutputHandler(std::function<void()> handler) {
    endOfOutput_ = std::move(handler);
}

bool PtyLink::sendInput(std::string_view bytes) {
    if (process_.closed() ||
        bytes.size() > maxPending - (pending_.size() - offset_)) {
        return false;
    }
    if (offset_ && offset_ >= pending_.size() / 2) {
        pending_.erase(0, offset_);
        offset_ = 0;
    }
    pending_.append(bytes);
    return true;
}

void PtyLink::resize(int columns, int rows, int, int) {
    process_.resize(columns, rows);
}

void PtyLink::close() {
    process_.terminate();
}

bool PtyLink::finished() const noexcept {
    return eof_ && status_.has_value();
}
std::optional<PtyProcess::ExitStatus> PtyLink::exitStatus() const noexcept {
    return status_;
}

void PtyLink::pump(std::size_t readBudget, std::size_t writeBudget) {
    std::array<char, 4096> buffer{};
    while (readBudget && !eof_) {
        const auto result =
            process_.read(buffer.data(), std::min(buffer.size(), readBudget));
        if (result.state == PtyProcess::ReadState::Data) {
            if (events_.output) {
                events_.output(std::string_view(buffer.data(), result.count));
            }
            readBudget -= result.count;
        }
        else {
            if (result.state == PtyProcess::ReadState::Closed) {
                eof_ = true;
                if (endOfOutput_) {
                    endOfOutput_();
                }
            }
            break;
        }
    }
    while (writeBudget && offset_ < pending_.size() && !process_.closed()) {
        const auto count = process_.write(std::string_view(pending_).substr(
            offset_, std::min(writeBudget, pending_.size() - offset_)));
        if (!count) {
            break;
        }
        offset_ += count;
        writeBudget -= count;
    }
    if (offset_ == pending_.size() || process_.closed()) {
        pending_.clear();
        offset_ = 0;
    }
    status_ = process_.pollExit();
    if (finished() && !exitedReported_) {
        exitedReported_ = true;
        if (events_.exited) {
            events_.exited(status_->code, status_->signal);
        }
    }
}
