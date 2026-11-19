#include "ptysession.h"

#include "terminalmodel.h"
#include "vtparser.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::size_t maxPending = 1024 * 1024;
std::size_t validated(int value) {
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument("Invalid PTY session size");
    }
    return std::size_t(value);
}
} // namespace

struct PtySession::Impl {
    TerminalModel model;
    PtyProcess process;
    VtEvents events;
    std::string pending;
    std::size_t offset = 0;
    bool eof = false;
    bool replyOverflow = false;
    std::optional<PtyProcess::ExitStatus> status;
    VtParser parser;

    Impl(const std::vector<std::string> &arguments,
         int columns,
         int rows,
         VtEvents events,
         const std::string &term)
        : model(validated(columns), validated(rows), 1000)
        , process(arguments, columns, rows, term)
        , events(std::move(events))
        , parser(model,
                 {[this](std::string_view bytes) {
                      if (!send(bytes)) {
                          replyOverflow = true;
                      }
                      if (this->events.reply) {
                          this->events.reply(bytes);
                      }
                  },
                  [this](std::string_view title) {
                      if (this->events.title) {
                          this->events.title(title);
                      }
                  }},
                 {220, 225, 245},
                 {14, 17, 22}) {
        model.eraseDisplay(TerminalModel::EraseMode::All);
    }
    bool send(std::string_view bytes) {
        if (process.closed() ||
            bytes.size() > maxPending - (pending.size() - offset)) {
            return false;
        }
        if (offset && offset >= pending.size() / 2) {
            pending.erase(0, offset);
            offset = 0;
        }
        pending.append(bytes);
        return true;
    }
    void pump(std::size_t readBudget, std::size_t writeBudget) {
        std::array<char, 4096> buffer{};
        while (readBudget && !eof) {
            const auto result = process.read(
                buffer.data(), std::min(buffer.size(), readBudget));
            if (result.state == PtyProcess::ReadState::Data) {
                parser.feed(std::string_view(buffer.data(), result.count));
                readBudget -= result.count;
            }
            else {
                if (result.state == PtyProcess::ReadState::Closed) {
                    parser.finish();
                    eof = true;
                }
                break;
            }
        }
        while (writeBudget && offset < pending.size() && !process.closed()) {
            const auto count = process.write(std::string_view(pending).substr(
                offset, std::min(writeBudget, pending.size() - offset)));
            if (!count) {
                break;
            }
            offset += count;
            writeBudget -= count;
        }
        if (offset == pending.size() || process.closed()) {
            pending.clear();
            offset = 0;
        }
        status = process.pollExit();
        if (replyOverflow) {
            throw std::length_error("PTY reply queue limit exceeded");
        }
    }
};

PtySession::PtySession(const std::vector<std::string> &arguments,
                       int columns,
                       int rows,
                       VtEvents events,
                       const std::string &term)
    : impl_(std::make_unique<Impl>(
          arguments, columns, rows, std::move(events), term)) {}
PtySession::~PtySession() = default;
TerminalModel &PtySession::model() noexcept {
    return impl_->model;
}
const TerminalModes &PtySession::modes() const noexcept {
    return impl_->parser.modes();
}
bool PtySession::send(std::string_view bytes) {
    return impl_->send(bytes);
}
void PtySession::pump(std::size_t readBudget, std::size_t writeBudget) {
    impl_->pump(readBudget, writeBudget);
}
void PtySession::resize(int columns, int rows) {
    if (columns <= 0 || columns > 65535 || rows <= 0 || rows > 65535) {
        throw std::invalid_argument(
            "PTY session dimensions must be in 1..65535");
    }
    impl_->model.resize(std::size_t(columns), std::size_t(rows));
    impl_->process.resize(columns, rows);
}
bool PtySession::finished() const noexcept {
    return impl_->eof && impl_->status.has_value();
}
std::optional<PtyProcess::ExitStatus> PtySession::exitStatus() const noexcept {
    return impl_->status;
}
